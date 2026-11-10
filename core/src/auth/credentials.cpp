#include "hubbo/auth/credentials.h"

#include <cstring>

#include "hubbo/domain/ids.h"

namespace hubbo::auth {

namespace {

constexpr char kAdminKey[] = "admin";  // "<username>|<password hash>"
constexpr char kTokenPrefix[] = "tok_";

bool readable(storage::Health health) {
    return health == storage::Health::Available || health == storage::Health::ReadOnly;
}

// Token record: "<sha256 base64url>|<created unix or -1>|<name>".
std::string encodeRecord(const uint8_t hash[kSha256Size], int64_t created, std::string_view name) {
    return base64UrlEncode(hash, kSha256Size) + "|" + std::to_string(created) + "|" + std::string(name);
}

bool decodeRecord(std::string_view text, uint8_t hash[kSha256Size], int64_t *created, std::string *name) {
    const size_t a = text.find('|');
    const size_t b = a == std::string_view::npos ? a : text.find('|', a + 1);
    if (b == std::string_view::npos) return false;
    size_t length = 0;
    if (!base64UrlDecode(text.substr(0, a), hash, kSha256Size, &length) || length != kSha256Size) return false;
    const std::string_view number = text.substr(a + 1, b - a - 1);
    if (number.empty() || number.size() > 20) return false;
    int64_t value = 0;
    bool negative = false;
    for (size_t i = 0; i < number.size(); i++) {
        if (i == 0 && number[i] == '-') {
            negative = true;
            continue;
        }
        if (number[i] < '0' || number[i] > '9') return false;
        value = value * 10 + (number[i] - '0');
    }
    *created = negative ? -value : value;
    *name = std::string(text.substr(b + 1));
    return isValidName(*name);
}

}  // namespace

bool isValidUsername(std::string_view username) {
    if (username.empty() || username.size() > kMaxUsernameLength) return false;
    for (char c : username) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                        c == '_' || c == '.' || c == '-';
        if (!ok) return false;
    }
    return true;
}

Credentials::Credentials(SecretStore &store, Hash &hash, RandomSource &random, uint32_t passwordIterations)
    : store_(store), hash_(hash), random_(random), iterations_(passwordIterations) {}

std::string Credentials::tokenKey(std::string_view id) { return kTokenPrefix + std::string(id); }

const char *credentialStateName(CredentialState state) {
    switch (state) {
        case CredentialState::Fresh: return "fresh";
        case CredentialState::Ok: return "ok";
        case CredentialState::Unavailable: break;
    }
    return "unavailable";
}

void Credentials::load() {
    username_.clear();
    passwordHash_.clear();
    tokens_.clear();
    if (!readable(store_.health())) {
        state_ = CredentialState::Unavailable;
        return;
    }
    std::string admin;
    const SecretRead adminRead = store_.read(kAdminKey, &admin);
    if (adminRead == SecretRead::Unavailable || adminRead == SecretRead::Corrupt) {
        state_ = CredentialState::Unavailable;
        return;
    }
    std::vector<std::string> tokenKeys;
    const SecretRead listed = store_.keys(kTokenPrefix, &tokenKeys);
    if (listed != SecretRead::Ok) {
        state_ = CredentialState::Unavailable;
        return;
    }
    for (const auto &key : tokenKeys) {
        std::string value;
        StoredToken token;
        const SecretRead r = store_.read(key, &value);
        if (r == SecretRead::Unavailable || r == SecretRead::Corrupt) {
            tokens_.clear();
            state_ = CredentialState::Unavailable;
            return;
        }
        if (r != SecretRead::Ok || !decodeRecord(value, token.hash, &token.createdAt, &token.name)) continue;
        token.id = key.substr(sizeof(kTokenPrefix) - 1);
        tokens_.push_back(std::move(token));
    }
    if (adminRead == SecretRead::NotFound) {
        // Tokens can only be made by an admin: tokens without one means the
        // admin record was lost, not that there never was one.
        state_ = tokenKeys.empty() ? CredentialState::Fresh : CredentialState::Unavailable;
        if (!tokenKeys.empty()) tokens_.clear();
        return;
    }
    const size_t bar = admin.find('|');
    const std::string user = admin.substr(0, bar);
    const std::string hash = bar == std::string::npos ? std::string() : admin.substr(bar + 1);
    if (!isValidUsername(user) || hash.rfind("pbkdf2-sha256$", 0) != 0) {
        tokens_.clear();
        state_ = CredentialState::Unavailable;
        return;
    }
    username_ = user;
    passwordHash_ = hash;
    state_ = CredentialState::Ok;
}

CredentialResult Credentials::adminGuard() const {
    switch (state_) {
        case CredentialState::Ok: return CredentialResult::Ok;
        case CredentialState::Fresh: return CredentialResult::NoAdmin;
        case CredentialState::Unavailable: break;
    }
    return CredentialResult::Unavailable;
}

CredentialResult Credentials::setupGuard() const {
    switch (state_) {
        case CredentialState::Fresh: return CredentialResult::Ok;
        case CredentialState::Ok: return CredentialResult::AlreadyExists;
        case CredentialState::Unavailable: break;
    }
    return CredentialResult::Unavailable;
}

CredentialResult Credentials::setupAdmin(std::string_view username, std::string_view password) {
    if (const CredentialResult guard = setupGuard(); guard != CredentialResult::Ok) return guard;
    if (!isValidUsername(username)) return CredentialResult::InvalidUsername;
    if (!isAcceptablePassword(password)) return CredentialResult::WeakPassword;
    std::string encoded;
    if (!hashPassword(hash_, random_, password, iterations_, &encoded)) return CredentialResult::NoEntropy;
    if (!store_.write(kAdminKey, std::string(username) + "|" + encoded)) return CredentialResult::StorageFailed;
    username_ = std::string(username);
    passwordHash_ = encoded;
    state_ = CredentialState::Ok;
    return CredentialResult::Ok;
}

bool Credentials::verifyLogin(std::string_view username, std::string_view password) {
    const bool known = state_ == CredentialState::Ok && username == username_;
    const PasswordCheck check = verifyPassword(hash_, password, known ? passwordHash_ : std::string(dummyPasswordHash()));
    return known && check == PasswordCheck::Match;
}

CredentialResult Credentials::changePassword(std::string_view current, std::string_view next) {
    if (const CredentialResult guard = adminGuard(); guard != CredentialResult::Ok) return guard;
    if (!verifyLogin(username_, current)) return CredentialResult::WrongPassword;
    if (!isAcceptablePassword(next)) return CredentialResult::WeakPassword;
    std::string encoded;
    if (!hashPassword(hash_, random_, next, iterations_, &encoded)) return CredentialResult::NoEntropy;
    if (!store_.write(kAdminKey, username_ + "|" + encoded)) return CredentialResult::StorageFailed;
    passwordHash_ = encoded;
    return CredentialResult::Ok;
}

CredentialResult Credentials::createToken(std::string_view name, int64_t nowUnix, IssuedToken *issued) {
    if (const CredentialResult guard = adminGuard(); guard != CredentialResult::Ok) return guard;
    if (!isValidName(name)) return CredentialResult::InvalidName;
    if (tokens_.size() >= kMaxApiTokens) return CredentialResult::LimitReached;
    IssuedToken fresh;
    do {
        if (!issueToken(hash_, random_, &fresh)) return CredentialResult::NoEntropy;
    } while (hasTokenId(fresh.id));
    if (!store_.write(tokenKey(fresh.id), encodeRecord(fresh.hash, nowUnix, name))) {
        return CredentialResult::StorageFailed;
    }
    StoredToken token;
    token.id = fresh.id;
    token.name = std::string(name);
    std::memcpy(token.hash, fresh.hash, kSha256Size);
    token.createdAt = nowUnix;
    tokens_.push_back(std::move(token));
    *issued = std::move(fresh);
    return CredentialResult::Ok;
}

CredentialResult Credentials::revokeToken(std::string_view id) {
    if (const CredentialResult guard = adminGuard(); guard != CredentialResult::Ok) return guard;
    for (size_t i = 0; i < tokens_.size(); i++) {
        if (tokens_[i].id != id) continue;
        if (!store_.erase(tokenKey(id))) return CredentialResult::StorageFailed;
        tokens_.erase(tokens_.begin() + static_cast<std::ptrdiff_t>(i));
        return CredentialResult::Ok;
    }
    return CredentialResult::NotFound;
}

const StoredToken *Credentials::authenticateToken(std::string_view presented, int64_t nowUnix) {
    const StoredToken *found = findToken(hash_, presented, tokens_.data(), tokens_.size());
    if (found == nullptr) return nullptr;
    StoredToken &token = tokens_[static_cast<size_t>(found - tokens_.data())];
    token.lastUsedAt = nowUnix;
    return &token;
}

bool Credentials::hasTokenId(std::string_view id) const {
    for (const auto &t : tokens_) {
        if (t.id == id) return true;
    }
    return false;
}

}  // namespace hubbo::auth
