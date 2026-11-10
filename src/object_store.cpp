#include "aowis/config/object_store.h"

#include "aowis/config/json.h"
#include "json_internal.h"
#include "memory_backend.h"
#include "storage_backend.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace aowis::config {
namespace {
using detail::max_record_bytes;

bool validRevision(std::string_view revision)
{
    if (revision.size() != 32) return false;
    for (char value : revision) {
        if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'))) return false;
    }
    return true;
}

Result<bool> validatePayload(std::string_view payload)
{
    if (payload.size() > max_record_bytes - 512) {
        return Result<bool>::failure({ErrorCode::object_too_large, "Object payload exceeds size limit"});
    }
    return validateJsonObject(payload);
}

std::string encodeRecord(const StoredObject &record)
{
    std::string text;
    text.reserve(record.json.size() + 160);
    text += "{\"envelope_version\":1,\"id\":\"";
    text += record.info.id.value();
    text += "\",\"revision\":\"";
    text += record.info.revision;
    text += "\",\"payload\":";
    text += record.json;
    text += "}\n";
    return text;
}

Result<StoredObject> decodeRecord(std::string_view text, const ObjectId &requested)
{
    Result<std::vector<detail::JsonMember>> members = detail::parseJsonObject(text);
    if (!members) {
        if (members.error().code == ErrorCode::object_too_large) {
            return Result<StoredObject>::failure(members.error());
        }
        return Result<StoredObject>::failure({ErrorCode::corrupt_object, "Stored object is not valid JSON"});
    }
    std::string_view schema;
    std::string_view id;
    std::string_view revision;
    std::string_view payload;
    for (const detail::JsonMember &member : members.value()) {
        std::string_view *destination = nullptr;
        if (member.key == "envelope_version") destination = &schema;
        else if (member.key == "id") destination = &id;
        else if (member.key == "revision") destination = &revision;
        else if (member.key == "payload") destination = &payload;
        if (destination == nullptr || !destination->empty()) {
            return Result<StoredObject>::failure({ErrorCode::corrupt_object, "Stored object has invalid or duplicate fields"});
        }
        *destination = member.value;
    }
    if (schema.empty() || id.size() < 2 || revision.size() < 2 || payload.empty()) {
        return Result<StoredObject>::failure({ErrorCode::corrupt_object, "Stored object is missing required fields"});
    }
    if (schema != "1") {
        // Recognize syntactically valid integer versions, so future formats
        // fail explicitly instead of being mistaken for damaged records.
        std::uint32_t parsed_version = 0;
        const std::from_chars_result parsed = std::from_chars(
            schema.data(), schema.data() + schema.size(), parsed_version);
        if (parsed.ec == std::errc{} && parsed.ptr == schema.data() + schema.size()) {
            return Result<StoredObject>::failure({ErrorCode::unsupported_schema, "Stored object uses an unsupported schema version"});
        }
        return Result<StoredObject>::failure({ErrorCode::corrupt_object, "Stored object has an invalid schema version"});
    }
    if (id.front() != '"' || id.back() != '"' || revision.front() != '"'
        || revision.back() != '"') {
        return Result<StoredObject>::failure({ErrorCode::corrupt_object, "Stored object identity fields are not strings"});
    }
    id.remove_prefix(1); id.remove_suffix(1);
    revision.remove_prefix(1); revision.remove_suffix(1);
    Result<ObjectId> parsed_id = ObjectId::parse(id);
    if (!parsed_id || parsed_id.value().value() != requested.value() || !validRevision(revision)) {
        return Result<StoredObject>::failure({ErrorCode::corrupt_object, "Stored object identity or revision is invalid"});
    }
    if (payload.front() != '{') {
        return Result<StoredObject>::failure({ErrorCode::corrupt_object, "Stored object payload must be a JSON object"});
    }
    return Result<StoredObject>::success({{parsed_id.value(), std::string(revision)}, std::string(payload)});
}

Result<std::string> newRevision()
{
    Result<ObjectId> generated = ObjectId::generate();
    if (!generated) return Result<std::string>::failure(generated.error());
    std::string revision;
    for (char value : generated.value().value()) {
        if (value != '-') revision += value;
    }
    return Result<std::string>::success(std::move(revision));
}

Result<StoredObject> loadRecord(const detail::CollectionSession &session, const ObjectId &id)
{
    Result<std::string> text = session.readEntry(id);
    if (!text) return Result<StoredObject>::failure(text.error());
    return decodeRecord(text.value(), id);
}

// Backend-level failures abort; per-entry failures are reported as
// UnreadableObject so one damaged entry cannot hide every other record (and
// the damaged one stays visible for repair).
Result<ObjectSnapshot> enumerate(const detail::CollectionSession &session, bool keep_payload)
{
    Result<std::vector<std::string>> names = session.entryNames();
    if (!names) return Result<ObjectSnapshot>::failure(names.error());
    ObjectSnapshot snapshot;
    for (const std::string &name : names.value()) {
        Result<ObjectId> id = ObjectId::parse(std::string_view(name).substr(0, name.size() - 5));
        if (!id) {
            snapshot.unreadable.push_back({name, std::nullopt,
                {ErrorCode::corrupt_object, "Collection entry is not a UUIDv7 object filename"}});
            continue;
        }
        Result<StoredObject> record = loadRecord(session, id.value());
        if (!record) {
            snapshot.unreadable.push_back({name, id.value(), record.error()});
            continue;
        }
        if (!keep_payload) record.value().json.clear();
        snapshot.objects.push_back(std::move(record.value()));
    }
    std::sort(snapshot.objects.begin(), snapshot.objects.end(), [](const StoredObject &left, const StoredObject &right) {
        return left.info.id.value() < right.info.id.value();
    });
    std::sort(snapshot.unreadable.begin(), snapshot.unreadable.end(),
              [](const UnreadableObject &left, const UnreadableObject &right) {
        return left.filename < right.filename;
    });
    return Result<ObjectSnapshot>::success(std::move(snapshot));
}

Result<std::string> checkedCollectionName(std::string_view collection)
{
    Result<OwnerId> checked = OwnerId::parse(collection);
    if (!checked) return Result<std::string>::failure({ErrorCode::invalid_collection, "Invalid collection name"});
    return Result<std::string>::success(checked.value().value());
}

detail::BackendMode backendMode(AccessMode mode)
{
    return mode == AccessMode::read_only ? detail::BackendMode::read_only : detail::BackendMode::read_write;
}

// Runs one operation in its own short owner session.
template<typename T, typename Operation>
Result<T> withSession(const std::shared_ptr<const detail::OwnerBackend> &backend, Operation operation)
{
    Result<std::unique_ptr<detail::OwnerSessionImpl>> session = backend->lock();
    if (!session) return Result<T>::failure(session.error());
    return operation(*session.value());
}
} // namespace

// ---------------------------------------------------------------- OwnerSession

OwnerSession::OwnerSession(std::shared_ptr<const detail::OwnerBackend> backend,
                           std::unique_ptr<detail::OwnerSessionImpl> impl)
    : backend_(std::move(backend)), impl_(std::move(impl)) {}
OwnerSession::OwnerSession(OwnerSession &&other) noexcept = default;
OwnerSession &OwnerSession::operator=(OwnerSession &&other) noexcept = default;
OwnerSession::~OwnerSession() = default;

// ------------------------------------------------------------------ OwnerStore

Result<OwnerStore> OwnerStore::open(const ConfigRoot &root, OpenOptions options)
{
    Result<std::shared_ptr<const detail::OwnerBackend>> backend =
        detail::openNativeBackend(root.path(), backendMode(options.mode), options.lock_timeout);
    if (!backend) return Result<OwnerStore>::failure(backend.error());
    // Verify the owner root (and, read_write, create it and its lock) now.
    Result<std::unique_ptr<detail::OwnerSessionImpl>> session = backend.value()->lock();
    if (!session) return Result<OwnerStore>::failure(session.error());
    return Result<OwnerStore>::success(OwnerStore(std::move(backend.value())));
}

Result<OwnerStore> OwnerStore::openInMemory(const MemoryStorage &storage, OpenOptions options)
{
    return Result<OwnerStore>::success(
        OwnerStore(detail::openMemoryBackend(storage.state_, backendMode(options.mode), options.lock_timeout)));
}

Result<ObjectCollection> OwnerStore::collection(std::string_view name) const
{
    Result<std::string> checked = checkedCollectionName(name);
    if (!checked) return Result<ObjectCollection>::failure(checked.error());
    Result<bool> verified = withSession<bool>(this->backend_, [&checked](detail::OwnerSessionImpl &session) {
        Result<std::unique_ptr<detail::CollectionSession>> collection = session.collection(checked.value(), true);
        if (!collection) return Result<bool>::failure(collection.error());
        return Result<bool>::success(true);
    });
    if (!verified) return Result<ObjectCollection>::failure(verified.error());
    return Result<ObjectCollection>::success(ObjectCollection(this->backend_, std::move(checked.value())));
}

Result<OwnerSession> OwnerStore::lock() const
{
    Result<std::unique_ptr<detail::OwnerSessionImpl>> session = this->backend_->lock();
    if (!session) return Result<OwnerSession>::failure(session.error());
    return Result<OwnerSession>::success(OwnerSession(this->backend_, std::move(session.value())));
}

AccessMode OwnerStore::mode() const noexcept
{
    return this->backend_->mode() == detail::BackendMode::read_only ? AccessMode::read_only : AccessMode::read_write;
}

// ------------------------------------------------------------ ObjectCollection
//
// Every operation is implemented once against a held OwnerSessionImpl; the
// session-less overloads wrap it in a short session of their own.

namespace {
Result<std::unique_ptr<detail::CollectionSession>> openCollection(detail::OwnerSessionImpl &session,
                                                        const std::string &name, bool recover)
{
    return session.collection(name, recover);
}

Result<ObjectSnapshot> readAllIn(detail::OwnerSessionImpl &owner, const std::string &name)
{
    Result<std::unique_ptr<detail::CollectionSession>> session = openCollection(owner, name, false);
    if (!session) return Result<ObjectSnapshot>::failure(session.error());
    return enumerate(*session.value(), true);
}

Result<ObjectListing> listIn(detail::OwnerSessionImpl &owner, const std::string &name)
{
    Result<std::unique_ptr<detail::CollectionSession>> session = openCollection(owner, name, false);
    if (!session) return Result<ObjectListing>::failure(session.error());
    Result<ObjectSnapshot> snapshot = enumerate(*session.value(), false);
    if (!snapshot) return Result<ObjectListing>::failure(snapshot.error());
    ObjectListing listing;
    listing.objects.reserve(snapshot.value().objects.size());
    for (StoredObject &record : snapshot.value().objects) listing.objects.push_back(std::move(record.info));
    listing.unreadable = std::move(snapshot.value().unreadable);
    return Result<ObjectListing>::success(std::move(listing));
}

Result<StoredObject> readIn(detail::OwnerSessionImpl &owner, const std::string &name, const ObjectId &id)
{
    Result<std::unique_ptr<detail::CollectionSession>> session = openCollection(owner, name, false);
    if (!session) return Result<StoredObject>::failure(session.error());
    return loadRecord(*session.value(), id);
}

Result<StoredObject> createIn(detail::OwnerSessionImpl &owner, const std::string &name, std::string_view json)
{
    Result<bool> checked = validatePayload(json);
    if (!checked) return Result<StoredObject>::failure(checked.error());
    Result<ObjectId> id = ObjectId::generate();
    if (!id) return Result<StoredObject>::failure(id.error());
    Result<std::string> revision = newRevision();
    if (!revision) return Result<StoredObject>::failure(revision.error());
    StoredObject record{{id.value(), revision.value()}, std::string(json)};
    Result<std::unique_ptr<detail::CollectionSession>> session = openCollection(owner, name, true);
    if (!session) return Result<StoredObject>::failure(session.error());
    Result<bool> stored = session.value()->writeEntry(record.info.id, encodeRecord(record), true);
    if (!stored) return Result<StoredObject>::failure(stored.error());
    return Result<StoredObject>::success(std::move(record));
}

Result<StoredObject> updateIn(detail::OwnerSessionImpl &owner, const std::string &name, const ObjectId &id,
                              std::string_view json, std::string_view expected_revision)
{
    Result<bool> checked = validatePayload(json);
    if (!checked) return Result<StoredObject>::failure(checked.error());
    if (!validRevision(expected_revision)) {
        return Result<StoredObject>::failure({ErrorCode::conflict, "A valid expected revision is required"});
    }
    Result<std::unique_ptr<detail::CollectionSession>> session = openCollection(owner, name, true);
    if (!session) return Result<StoredObject>::failure(session.error());
    Result<StoredObject> previous = loadRecord(*session.value(), id);
    if (!previous) return Result<StoredObject>::failure(previous.error());
    if (previous.value().info.revision != expected_revision) {
        return Result<StoredObject>::failure({ErrorCode::conflict, "Object changed since it was read"});
    }
    Result<std::string> revision = newRevision();
    if (!revision) return Result<StoredObject>::failure(revision.error());
    StoredObject updated{{id, revision.value()}, std::string(json)};
    Result<bool> stored = session.value()->writeEntry(id, encodeRecord(updated), false);
    if (!stored) return Result<StoredObject>::failure(stored.error());
    return Result<StoredObject>::success(std::move(updated));
}

Result<bool> eraseIn(detail::OwnerSessionImpl &owner, const std::string &name, const ObjectId &id,
                     std::string_view expected_revision)
{
    if (!validRevision(expected_revision)) {
        return Result<bool>::failure({ErrorCode::conflict, "A valid expected revision is required"});
    }
    Result<std::unique_ptr<detail::CollectionSession>> session = openCollection(owner, name, true);
    if (!session) return Result<bool>::failure(session.error());
    Result<StoredObject> previous = loadRecord(*session.value(), id);
    if (!previous) return Result<bool>::failure(previous.error());
    if (previous.value().info.revision != expected_revision) {
        return Result<bool>::failure({ErrorCode::conflict, "Object changed since it was read"});
    }
    return session.value()->removeEntry(id);
}

Result<bool> eraseCorruptIn(detail::OwnerSessionImpl &owner, const std::string &name, const ObjectId &id)
{
    Result<std::unique_ptr<detail::CollectionSession>> session = openCollection(owner, name, true);
    if (!session) return Result<bool>::failure(session.error());
    // Re-check inside the session: another instance may have repaired or
    // replaced the record since the caller saw it in an unreadable listing.
    Result<StoredObject> current = loadRecord(*session.value(), id);
    if (current) {
        return Result<bool>::failure({ErrorCode::conflict, "Object is readable; erase it with its revision"});
    }
    if (current.error().code != ErrorCode::corrupt_object) return Result<bool>::failure(current.error());
    return session.value()->removeEntry(id);
}

Error mismatch()
{
    return {ErrorCode::session_mismatch, "Session belongs to a different configuration store"};
}
} // namespace

Result<ObjectListing> ObjectCollection::list(const OwnerSession &session) const
{
    if (session.backend_ != this->backend_ || !session.impl_) return Result<ObjectListing>::failure(mismatch());
    return listIn(*session.impl_, this->name_);
}

Result<ObjectSnapshot> ObjectCollection::readAll(const OwnerSession &session) const
{
    if (session.backend_ != this->backend_ || !session.impl_) return Result<ObjectSnapshot>::failure(mismatch());
    return readAllIn(*session.impl_, this->name_);
}

Result<StoredObject> ObjectCollection::read(const OwnerSession &session, const ObjectId &id) const
{
    if (session.backend_ != this->backend_ || !session.impl_) return Result<StoredObject>::failure(mismatch());
    return readIn(*session.impl_, this->name_, id);
}

Result<StoredObject> ObjectCollection::create(const OwnerSession &session, std::string_view json) const
{
    if (session.backend_ != this->backend_ || !session.impl_) return Result<StoredObject>::failure(mismatch());
    return createIn(*session.impl_, this->name_, json);
}

Result<StoredObject> ObjectCollection::update(const OwnerSession &session, const ObjectId &id,
                                                std::string_view json, std::string_view expected_revision) const
{
    if (session.backend_ != this->backend_ || !session.impl_) return Result<StoredObject>::failure(mismatch());
    return updateIn(*session.impl_, this->name_, id, json, expected_revision);
}

Result<bool> ObjectCollection::erase(const OwnerSession &session, const ObjectId &id,
                                     std::string_view expected_revision) const
{
    if (session.backend_ != this->backend_ || !session.impl_) return Result<bool>::failure(mismatch());
    return eraseIn(*session.impl_, this->name_, id, expected_revision);
}

Result<bool> ObjectCollection::eraseCorrupt(const OwnerSession &session, const ObjectId &id) const
{
    if (session.backend_ != this->backend_ || !session.impl_) return Result<bool>::failure(mismatch());
    return eraseCorruptIn(*session.impl_, this->name_, id);
}

Result<ObjectListing> ObjectCollection::list() const
{
    return withSession<ObjectListing>(this->backend_, [this](detail::OwnerSessionImpl &owner) {
        return listIn(owner, this->name_);
    });
}

Result<ObjectSnapshot> ObjectCollection::readAll() const
{
    return withSession<ObjectSnapshot>(this->backend_, [this](detail::OwnerSessionImpl &owner) {
        return readAllIn(owner, this->name_);
    });
}

Result<StoredObject> ObjectCollection::read(const ObjectId &id) const
{
    return withSession<StoredObject>(this->backend_, [this, &id](detail::OwnerSessionImpl &owner) {
        return readIn(owner, this->name_, id);
    });
}

Result<StoredObject> ObjectCollection::create(std::string_view json) const
{
    return withSession<StoredObject>(this->backend_, [this, json](detail::OwnerSessionImpl &owner) {
        return createIn(owner, this->name_, json);
    });
}

Result<StoredObject> ObjectCollection::update(const ObjectId &id, std::string_view json,
                                                std::string_view expected_revision) const
{
    return withSession<StoredObject>(this->backend_, [this, &id, json, expected_revision](detail::OwnerSessionImpl &owner) {
        return updateIn(owner, this->name_, id, json, expected_revision);
    });
}

Result<bool> ObjectCollection::erase(const ObjectId &id, std::string_view expected_revision) const
{
    return withSession<bool>(this->backend_, [this, &id, expected_revision](detail::OwnerSessionImpl &owner) {
        return eraseIn(owner, this->name_, id, expected_revision);
    });
}

Result<bool> ObjectCollection::eraseCorrupt(const ObjectId &id) const
{
    return withSession<bool>(this->backend_, [this, &id](detail::OwnerSessionImpl &owner) {
        return eraseCorruptIn(owner, this->name_, id);
    });
}

} // namespace aowis::config
