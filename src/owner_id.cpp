#include "aowis/config/owner_id.h"

namespace aowis::config {

Result<OwnerId> OwnerId::parse(std::string_view text)
{
    // Portable lowercase ASCII kebab-case. Keep owner paths predictable
    // even when the caller supplies untrusted strings.
    if (text.empty() || text.size() > 63 || text.front() < 'a' || text.front() > 'z') {
        return Result<OwnerId>::failure({ErrorCode::invalid_owner_id,
                                         "Owner ID must start with a lowercase ASCII letter and be at most 63 characters"});
    }

    bool previous_was_hyphen = false;
    for (char letter : text) {
        const bool alphanumeric = (letter >= 'a' && letter <= 'z')
                                  || (letter >= '0' && letter <= '9');
        if (alphanumeric) {
            previous_was_hyphen = false;
        } else if (letter == '-' && !previous_was_hyphen) {
            previous_was_hyphen = true;
        } else {
            return Result<OwnerId>::failure({ErrorCode::invalid_owner_id,
                                             "Owner ID must use lowercase ASCII letters, digits and single interior hyphens"});
        }
    }
    if (previous_was_hyphen) {
        return Result<OwnerId>::failure({ErrorCode::invalid_owner_id,
                                         "Owner ID cannot end with a hyphen"});
    }
    return Result<OwnerId>::success(OwnerId(std::string(text)));
}

} // namespace aowis::config
