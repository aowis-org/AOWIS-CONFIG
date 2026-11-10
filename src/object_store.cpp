#include "aowis/config/object_store.h"

#include "aowis/config/json.h"
#include "json_internal.h"
#include "memory_backend.h"
#include "storage_backend.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <memory>
#include <cstdint>
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

Result<bool> validatePayload(std::string_view payload, std::uint32_t schema_version)
{
    if (schema_version != 1) {
        return Result<bool>::failure({ErrorCode::unsupported_schema, "Only object schema version 1 is supported"});
    }
    if (payload.size() > max_record_bytes - 512) {
        return Result<bool>::failure({ErrorCode::object_too_large, "Object payload exceeds size limit"});
    }
    return validateJsonObject(payload);
}

std::string encodeRecord(const StoredObject &record)
{
    std::string text;
    text.reserve(record.json.size() + 160);
    text += "{\"schema_version\":1,\"id\":\"";
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
        if (member.key == "schema_version") destination = &schema;
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
    return Result<StoredObject>::success({{parsed_id.value(), 1, std::string(revision)}, std::string(payload)});
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

// One locked pass. Backend-level failures abort; per-entry failures are
// reported as UnreadableObject so one damaged entry cannot hide every other
// record (and the damaged one stays visible for repair).
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
} // namespace

Result<ObjectCollection> ObjectCollection::fromBackend(
    std::shared_ptr<const detail::CollectionBackend> backend, std::string name)
{
    // Opening verifies the storage location and removes crash residue now,
    // so configuration problems surface at open() rather than on first use.
    Result<std::unique_ptr<detail::CollectionSession>> session = backend->lock(true);
    if (!session) return Result<ObjectCollection>::failure(session.error());
    return Result<ObjectCollection>::success(ObjectCollection(std::move(backend), std::move(name)));
}

Result<ObjectCollection> ObjectCollection::open(const ConfigRoot &root, std::string_view collection,
                                                std::chrono::milliseconds lock_timeout)
{
    Result<std::string> name = checkedCollectionName(collection);
    if (!name) return Result<ObjectCollection>::failure(name.error());
    Result<std::shared_ptr<const detail::CollectionBackend>> backend =
        detail::openNativeBackend(root.path(), name.value(), lock_timeout);
    if (!backend) return Result<ObjectCollection>::failure(backend.error());
    return ObjectCollection::fromBackend(std::move(backend.value()), std::move(name.value()));
}

Result<ObjectCollection> ObjectCollection::openInMemory(const MemoryStorage &storage, std::string_view collection,
                                                        std::chrono::milliseconds lock_timeout)
{
    Result<std::string> name = checkedCollectionName(collection);
    if (!name) return Result<ObjectCollection>::failure(name.error());
    // Build the backend first: argument evaluation order is unspecified, so
    // name must not be moved in the same call expression that reads it.
    std::shared_ptr<const detail::CollectionBackend> backend =
        detail::openMemoryBackend(storage.state_, name.value(), lock_timeout);
    return ObjectCollection::fromBackend(std::move(backend), std::move(name.value()));
}

Result<ObjectListing> ObjectCollection::list() const
{
    Result<ObjectSnapshot> snapshot = this->readAll();
    if (!snapshot) return Result<ObjectListing>::failure(snapshot.error());
    ObjectListing listing;
    listing.objects.reserve(snapshot.value().objects.size());
    for (StoredObject &record : snapshot.value().objects) listing.objects.push_back(std::move(record.info));
    listing.unreadable = std::move(snapshot.value().unreadable);
    return Result<ObjectListing>::success(std::move(listing));
}

Result<ObjectSnapshot> ObjectCollection::readAll() const
{
    Result<std::unique_ptr<detail::CollectionSession>> session = this->backend_->lock(false);
    if (!session) return Result<ObjectSnapshot>::failure(session.error());
    return enumerate(*session.value(), true);
}

Result<StoredObject> ObjectCollection::read(const ObjectId &id) const
{
    Result<std::unique_ptr<detail::CollectionSession>> session = this->backend_->lock(false);
    if (!session) return Result<StoredObject>::failure(session.error());
    return loadRecord(*session.value(), id);
}

Result<StoredObject> ObjectCollection::create(std::string_view json, std::uint32_t schema_version) const
{
    Result<bool> checked = validatePayload(json, schema_version);
    if (!checked) return Result<StoredObject>::failure(checked.error());
    Result<ObjectId> id = ObjectId::generate();
    if (!id) return Result<StoredObject>::failure(id.error());
    Result<std::string> revision = newRevision();
    if (!revision) return Result<StoredObject>::failure(revision.error());
    StoredObject record{{id.value(), schema_version, revision.value()}, std::string(json)};
    Result<std::unique_ptr<detail::CollectionSession>> session = this->backend_->lock(true);
    if (!session) return Result<StoredObject>::failure(session.error());
    Result<bool> stored = session.value()->writeEntry(record.info.id, encodeRecord(record), true);
    if (!stored) return Result<StoredObject>::failure(stored.error());
    return Result<StoredObject>::success(std::move(record));
}

Result<StoredObject> ObjectCollection::update(const ObjectId &id, std::string_view json,
                                                std::string_view expected_revision,
                                                std::uint32_t schema_version) const
{
    Result<bool> checked = validatePayload(json, schema_version);
    if (!checked) return Result<StoredObject>::failure(checked.error());
    if (!validRevision(expected_revision)) {
        return Result<StoredObject>::failure({ErrorCode::conflict, "A valid expected revision is required"});
    }
    Result<std::unique_ptr<detail::CollectionSession>> session = this->backend_->lock(true);
    if (!session) return Result<StoredObject>::failure(session.error());
    Result<StoredObject> previous = loadRecord(*session.value(), id);
    if (!previous) return Result<StoredObject>::failure(previous.error());
    if (previous.value().info.revision != expected_revision) {
        return Result<StoredObject>::failure({ErrorCode::conflict, "Object changed since it was read"});
    }
    Result<std::string> revision = newRevision();
    if (!revision) return Result<StoredObject>::failure(revision.error());
    StoredObject updated{{id, schema_version, revision.value()}, std::string(json)};
    Result<bool> stored = session.value()->writeEntry(id, encodeRecord(updated), false);
    if (!stored) return Result<StoredObject>::failure(stored.error());
    return Result<StoredObject>::success(std::move(updated));
}

Result<bool> ObjectCollection::erase(const ObjectId &id, std::string_view expected_revision) const
{
    if (!validRevision(expected_revision)) {
        return Result<bool>::failure({ErrorCode::conflict, "A valid expected revision is required"});
    }
    Result<std::unique_ptr<detail::CollectionSession>> session = this->backend_->lock(true);
    if (!session) return Result<bool>::failure(session.error());
    Result<StoredObject> previous = loadRecord(*session.value(), id);
    if (!previous) return Result<bool>::failure(previous.error());
    if (previous.value().info.revision != expected_revision) {
        return Result<bool>::failure({ErrorCode::conflict, "Object changed since it was read"});
    }
    return session.value()->removeEntry(id);
}

Result<bool> ObjectCollection::eraseCorrupt(const ObjectId &id) const
{
    Result<std::unique_ptr<detail::CollectionSession>> session = this->backend_->lock(true);
    if (!session) return Result<bool>::failure(session.error());
    // Re-check under the lock: another instance may have repaired or replaced
    // the record since the caller saw it in an UnreadableObject listing.
    Result<StoredObject> current = loadRecord(*session.value(), id);
    if (current) {
        return Result<bool>::failure({ErrorCode::conflict, "Object is readable; erase it with its revision"});
    }
    if (current.error().code != ErrorCode::corrupt_object) return Result<bool>::failure(current.error());
    return session.value()->removeEntry(id);
}

} // namespace aowis::config
