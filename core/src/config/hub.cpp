#include "hubbo/config/hub.h"

#include <cstdint>
#include <cstdio>
#include <utility>

namespace hubbo::config {

namespace {

// The code point at `text[*i]`, advancing `*i`; false if it isn't valid
// UTF-8 (overlong forms, surrogates and values past U+10FFFF included).
bool nextCodePoint(std::string_view text, size_t *i, uint32_t *out) {
    const auto byte = [&](size_t k) { return static_cast<uint8_t>(text[k]); };
    const uint8_t lead = byte(*i);
    size_t length = 0;
    uint32_t cp = 0;
    uint32_t min = 0;
    if (lead < 0x80) {
        length = 1;
        cp = lead;
    } else if ((lead & 0xE0) == 0xC0) {
        length = 2;
        cp = lead & 0x1F;
        min = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
        cp = lead & 0x0F;
        min = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
        cp = lead & 0x07;
        min = 0x10000;
    } else {
        return false;
    }
    if (*i + length > text.size()) return false;
    for (size_t k = 1; k < length; k++) {
        const uint8_t b = byte(*i + k);
        if ((b & 0xC0) != 0x80) return false;
        cp = (cp << 6) | (b & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    *i += length;
    *out = cp;
    return true;
}

bool isControl(uint32_t cp) { return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F); }

// A name's identity before the platform reports what mDNS advertises.
HubIdentity identityFor(std::string name) {
    HubIdentity identity;
    identity.hostname = hostnameFor(name);
    identity.name = std::move(name);
    return identity;
}

}  // namespace

std::string trimHubName(std::string_view requested) {
    while (!requested.empty() && requested.front() == ' ') requested.remove_prefix(1);
    while (!requested.empty() && requested.back() == ' ') requested.remove_suffix(1);
    return std::string(requested);
}

ErrorCode checkHubName(std::string_view name) {
    if (name.empty()) return ErrorCode::Required;
    if (name.size() > kHubNameMaxBytes) return ErrorCode::TooLong;
    size_t chars = 0;
    for (size_t i = 0; i < name.size(); chars++) {
        uint32_t cp = 0;
        if (!nextCodePoint(name, &i, &cp) || isControl(cp)) return ErrorCode::InvalidName;
    }
    if (chars > kHubNameMaxChars) return ErrorCode::TooLong;
    if (name.front() == ' ' || name.back() == ' ') return ErrorCode::InvalidName;
    return ErrorCode::Ok;
}

std::string hostnameFor(std::string_view name) {
    std::string out;
    bool dash = false;  // a run of other characters is pending
    for (const char c : name) {
        const bool lower = c >= 'a' && c <= 'z';
        const bool upper = c >= 'A' && c <= 'Z';
        const bool digit = c >= '0' && c <= '9';
        if (!lower && !upper && !digit) {
            dash = true;
            continue;
        }
        if (dash && !out.empty()) out += '-';
        dash = false;
        out += upper ? static_cast<char>(c - 'A' + 'a') : c;
    }
    if (out.size() > kHostnameMaxLength) out.resize(kHostnameMaxLength);
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? kDefaultHostname : out;
}

std::string defaultHubName(const uint8_t mac[6]) {
    char name[16];
    std::snprintf(name, sizeof name, "%s %02X%02X", kDefaultHubName, mac[4], mac[5]);
    return name;
}

HubSettings::HubSettings(storage::StateStore &store, std::string defaultName)
    : store_(store), defaultName_(std::move(defaultName)), identity_(identityFor(defaultName_)) {}

ErrorCode HubSettings::load() {
    std::string check;
    const bool off = store_.get(kCheckUpdatesKey, &check) == ErrorCode::Ok && check == "0";
    {
        std::lock_guard<std::mutex> lock(mutex_);
        checkUpdates_ = !off;
    }
    std::string saved;
    ErrorCode read = store_.get(kHubNameKey, &saved);
    if (read == ErrorCode::Ok && checkHubName(saved) != ErrorCode::Ok) read = ErrorCode::Corrupt;
    std::lock_guard<std::mutex> lock(mutex_);
    matchesStore_ = read == ErrorCode::Ok || read == ErrorCode::NotFound;
    if (read != ErrorCode::Ok) {
        identity_ = identityFor(defaultName_);  // nothing is written over what's saved
        return read;
    }
    identity_ = identityFor(std::move(saved));
    return ErrorCode::Ok;
}

HubIdentity HubSettings::identity() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return identity_;
}

ErrorCode HubSettings::rename(std::string_view requested, HubIdentity *out) {
    std::string name = trimHubName(requested);
    const ErrorCode checked = checkHubName(name);
    if (checked != ErrorCode::Ok) return checked;
    HubIdentity next = identityFor(std::move(name));

    std::lock_guard<std::mutex> renaming(saveMutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (matchesStore_ && identity_.name == next.name) {  // no flash write, nothing to re-apply
            if (out != nullptr) *out = identity_;  // the advertised hostname still holds
            return ErrorCode::Ok;
        }
    }
    if (!store_.set(kHubNameKey, next.name)) return ErrorCode::StorageFailed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        identity_ = next;
        matchesStore_ = true;
    }
    if (changed_) changed_(next);
    if (out != nullptr) *out = std::move(next);
    return ErrorCode::Ok;
}

ErrorCode HubSettings::forget() {
    std::lock_guard<std::mutex> renaming(saveMutex_);
    const bool nameRemoved = store_.remove(kHubNameKey);
    const bool removed = store_.remove(kCheckUpdatesKey) && nameRemoved;
    std::lock_guard<std::mutex> lock(mutex_);
    identity_ = identityFor(defaultName_);
    checkUpdates_ = true;
    matchesStore_ = removed;
    return removed ? ErrorCode::Ok : ErrorCode::StorageFailed;
}

void HubSettings::setAdvertisedHostname(std::string_view hostname) {
    std::lock_guard<std::mutex> lock(mutex_);  // never saveMutex_ (see hub.h)
    identity_.advertisedHostname.assign(hostname.data(), hostname.size());
}

bool HubSettings::checkUpdates() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return checkUpdates_;
}

ErrorCode HubSettings::setCheckUpdates(bool on) {
    std::lock_guard<std::mutex> saving(saveMutex_);
    if (!store_.set(kCheckUpdatesKey, on ? "1" : "0")) return ErrorCode::StorageFailed;
    std::lock_guard<std::mutex> lock(mutex_);
    checkUpdates_ = on;
    return ErrorCode::Ok;
}

void HubSettings::onChanged(Changed changed) { changed_ = std::move(changed); }

}  // namespace hubbo::config
