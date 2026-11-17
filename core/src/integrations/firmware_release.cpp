#include "hubbo/integrations/firmware_release.h"

#include <ArduinoJson.h>

namespace hubbo::release {

bool parseVersion(std::string_view text, Version *out) {
    uint16_t parts[3] = {};
    size_t at = 0;
    for (size_t p = 0; p < 3; p++) {
        if (p != 0) {
            if (at >= text.size() || text[at] != '.') return false;
            at++;
        }
        const size_t start = at;
        uint32_t value = 0;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
            value = value * 10 + static_cast<uint32_t>(text[at] - '0');
            if (value > UINT16_MAX) return false;
            at++;
        }
        const size_t digits = at - start;
        if (digits == 0 || (digits > 1 && text[start] == '0')) return false;
        parts[p] = static_cast<uint16_t>(value);
    }
    if (at != text.size()) return false;
    *out = Version{parts[0], parts[1], parts[2]};
    return true;
}

int compareVersions(const Version &a, const Version &b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

namespace {

bool isLowerHex(std::string_view s) {
    for (const char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool isVersion(std::string_view text) {
    Version v;
    return parseVersion(text, &v);
}

}  // namespace

bool parseManifest(std::string_view json, size_t slotBytes, Manifest *out, std::string *badField) {
    const auto bad = [&](const char *field) {
        if (badField != nullptr) *badField = field;
        return false;
    };
    if (json.size() > kMaxManifestBytes) return bad("");
    JsonDocument doc;
    if (deserializeJson(doc, json.data(), json.size(), DeserializationOption::NestingLimit(2)) !=
            DeserializationError::Ok ||
        !doc.is<JsonObjectConst>()) {
        return bad("");
    }
    const JsonObjectConst o = doc.as<JsonObjectConst>();
    Manifest m;

    const JsonVariantConst version = o["version"];
    if (!version.is<const char *>() || !isVersion(version.as<const char *>())) return bad("version");
    m.version = version.as<const char *>();

    const JsonVariantConst board = o["board"];
    if (!board.is<const char *>()) return bad("board");
    m.board = board.as<const char *>();
    if (m.board.empty() || m.board.size() > kMaxBoardLength) return bad("board");

    const JsonVariantConst layout = o["layout"];
    if (!layout.is<int64_t>() || layout.as<int64_t>() < 1 || layout.as<int64_t>() > UINT16_MAX) return bad("layout");
    m.layout = static_cast<uint16_t>(layout.as<int64_t>());

    const JsonVariantConst size = o["size"];
    if (!size.is<int64_t>() || size.as<int64_t>() < 1 || static_cast<uint64_t>(size.as<int64_t>()) > slotBytes) {
        return bad("size");
    }
    m.size = static_cast<size_t>(size.as<int64_t>());

    const JsonVariantConst sha = o["sha256"];
    if (!sha.is<const char *>()) return bad("sha256");
    m.sha256 = sha.as<const char *>();
    if (m.sha256.size() != kSha256HexLength || !isLowerHex(m.sha256)) return bad("sha256");

    const JsonVariantConst min = o["min_version"];
    if (!min.isUnbound()) {  // present: a version, never null
        if (!min.is<const char *>() || !isVersion(min.as<const char *>())) return bad("min_version");
        m.minVersion = min.as<const char *>();
    }

    *out = std::move(m);
    return true;
}

const char *availabilityName(Availability availability) {
    switch (availability) {
        case Availability::Available: return "available";
        case Availability::DevelopmentBuild: return "development_build";
        case Availability::WrongBoard: return "wrong_board";
        case Availability::WrongLayout: return "wrong_layout";
        case Availability::NotNewer: return "not_newer";
        case Availability::NeedsNewerBase: return "needs_newer_base";
    }
    return "not_newer";
}

Availability availability(const RunningFirmware &running, const Manifest &manifest) {
    Version current;
    if (!parseVersion(running.version, &current)) return Availability::DevelopmentBuild;
    if (manifest.board != running.board) return Availability::WrongBoard;
    if (manifest.layout != running.layout) return Availability::WrongLayout;
    Version offered;
    if (!parseVersion(manifest.version, &offered) || compareVersions(offered, current) <= 0) {
        return Availability::NotNewer;
    }
    Version min;
    if (!manifest.minVersion.empty() && parseVersion(manifest.minVersion, &min) &&
        compareVersions(current, min) < 0) {
        return Availability::NeedsNewerBase;
    }
    return Availability::Available;
}

std::string sha256Hex(std::string_view data, auth::Hash &hash) {
    uint8_t digest[auth::kSha256Size];
    hash.sha256(reinterpret_cast<const uint8_t *>(data.data()), data.size(), digest);
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(kSha256HexLength);
    for (const uint8_t byte : digest) {
        out += kHex[byte >> 4];
        out += kHex[byte & 0xF];
    }
    return out;
}

ImageCheck checkImage(std::string_view image, const Manifest &manifest, auth::Hash &hash) {
    if (image.size() != manifest.size) return ImageCheck::WrongSize;
    return sha256Hex(image, hash) == manifest.sha256 ? ImageCheck::Ok : ImageCheck::WrongHash;
}

const char *checkErrorName(CheckError error) {
    switch (error) {
        case CheckError::None: return nullptr;
        case CheckError::Unreachable: return "unreachable";
        case CheckError::BadSignature: return "bad_signature";
        case CheckError::BadManifest: return "bad_manifest";
    }
    return nullptr;
}

CheckOutcome checkForUpdate(ReleaseFeed &feed, const RunningFirmware &running) {
    CheckOutcome outcome;
    std::string bytes;
    switch (feed.fetchManifest(&bytes)) {
        case FetchResult::Ok: break;
        case FetchResult::Unreachable: outcome.error = CheckError::Unreachable; return outcome;
        case FetchResult::BadSignature: outcome.error = CheckError::BadSignature; return outcome;
    }
    Manifest manifest;
    if (!parseManifest(bytes, running.slotBytes, &manifest)) {
        outcome.error = CheckError::BadManifest;
        return outcome;
    }
    outcome.availability = availability(running, manifest);
    if (outcome.availability == Availability::Available) outcome.available = std::move(manifest);
    return outcome;
}

DownloadResult downloadImage(ReleaseFeed &feed, const Manifest &manifest, uint8_t *buffer, size_t capacity,
                             auth::Hash &hash, size_t *length) {
    if (feed.fetchImage(manifest, buffer, capacity, length) != FetchResult::Ok) return DownloadResult::Failed;
    const std::string_view image(reinterpret_cast<const char *>(buffer), *length);
    return checkImage(image, manifest, hash) == ImageCheck::Ok ? DownloadResult::Ok : DownloadResult::Mismatch;
}

UpdateCheckStatus UpdateCheckState::read() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

bool UpdateCheckState::beginCheck() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_.checking) return false;
    status_.checking = true;
    return true;
}

void UpdateCheckState::finishCheck(CheckOutcome outcome, uint32_t nowMs) {
    std::shared_ptr<const Manifest> available;  // built outside the lock
    if (outcome.available) available = std::make_shared<const Manifest>(std::move(*outcome.available));
    std::lock_guard<std::mutex> lock(mutex_);
    status_.checking = false;
    status_.checked = true;
    status_.checkedAtMs = nowMs;
    status_.error = outcome.error;
    if (outcome.error == CheckError::None) status_.available = std::move(available);
}

void UpdateCheckState::forgetChecks() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string rolledBackFrom = std::move(status_.rolledBackFrom);
    status_ = UpdateCheckStatus{};
    status_.rolledBackFrom = std::move(rolledBackFrom);
}

void UpdateCheckState::setRolledBackFrom(std::string_view version) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.rolledBackFrom.assign(version);
}

void UpdateCheckState::clearRolledBackFrom() {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.rolledBackFrom.clear();
}

std::optional<Manifest> UpdateCheckState::availableAs(std::string_view version) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!status_.available || status_.available->version != version) return std::nullopt;
    return *status_.available;
}

}  // namespace hubbo::release
