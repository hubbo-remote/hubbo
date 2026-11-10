#include "hubbo/auth/session.h"

#include <cstring>

namespace hubbo::auth {

namespace {

// Decodes a presented secret into exactly kSessionSecretBytes, or fails.
bool decodeSecret(std::string_view text, uint8_t out[kSessionSecretBytes]) {
    size_t length = 0;
    return text.size() == kSessionSecretLength &&
           base64UrlDecode(text, out, kSessionSecretBytes, &length) &&
           length == kSessionSecretBytes;
}

}  // namespace

SessionStore::SessionStore(RandomSource &random, uint64_t lifetimeMs) : random_(random), lifetimeMs_(lifetimeMs) {}

bool SessionStore::live(const Slot &slot, uint64_t nowMs) const {
    if (!slot.used || nowMs < slot.createdMs) {
        return false;  // clock went backwards: treat as expired
    }
    return nowMs - slot.createdMs < lifetimeMs_;
}

void SessionStore::wipe(Slot &slot) {
    secureZero(slot.id, sizeof(slot.id));
    slot.used = false;
    slot.createdMs = 0;
}

bool SessionStore::create(uint64_t nowMs, std::string *id) {
    Slot fresh;
    if (!random_.fill(fresh.id, sizeof(fresh.id))) {
        return false;
    }
    fresh.used = true;
    fresh.createdMs = nowMs;

    // Free or expired slot first, otherwise the oldest.
    size_t target = 0;
    for (size_t i = 0; i < kMaxSessions; i++) {
        if (!live(slots_[i], nowMs)) {
            target = i;
            break;
        }
        if (slots_[i].createdMs < slots_[target].createdMs) target = i;
    }
    wipe(slots_[target]);
    slots_[target] = fresh;
    *id = base64UrlEncode(fresh.id, sizeof(fresh.id));
    wipe(fresh);
    return true;
}

int SessionStore::find(std::string_view id, uint64_t nowMs) const {
    uint8_t presented[kSessionSecretBytes];
    if (!decodeSecret(id, presented)) {
        return -1;
    }
    int match = -1;
    for (size_t i = 0; i < kMaxSessions; i++) {
        const bool same = constantTimeEqual(presented, slots_[i].id, kSessionSecretBytes);
        if (same && slots_[i].used && match < 0) {
            match = static_cast<int>(i);
        }
    }
    secureZero(presented, sizeof(presented));
    if (match >= 0 && !live(slots_[match], nowMs)) {
        return -1;
    }
    return match;
}

SessionAuth SessionStore::authenticate(std::string_view id, uint64_t nowMs) const {
    SessionAuth result;
    const int index = find(id, nowMs);
    if (index < 0) {
        return result;
    }
    result.valid = true;
    result.expiresInMs = lifetimeMs_ - (nowMs - slots_[index].createdMs);
    return result;
}

void SessionStore::remove(std::string_view id) {
    uint8_t presented[kSessionSecretBytes];
    if (!decodeSecret(id, presented)) {
        return;
    }
    for (auto &slot : slots_) {
        if (slot.used && constantTimeEqual(presented, slot.id, kSessionSecretBytes)) {
            wipe(slot);
        }
    }
    secureZero(presented, sizeof(presented));
}

void SessionStore::removeAllExcept(std::string_view keepId) {
    uint8_t keep[kSessionSecretBytes] = {};
    const bool haveKeep = decodeSecret(keepId, keep);
    for (auto &slot : slots_) {
        if (!slot.used) continue;
        if (haveKeep && constantTimeEqual(keep, slot.id, kSessionSecretBytes)) continue;
        wipe(slot);
    }
    secureZero(keep, sizeof(keep));
}

void SessionStore::clear() {
    for (auto &slot : slots_) {
        wipe(slot);
    }
}

size_t SessionStore::activeCount(uint64_t nowMs) const {
    size_t n = 0;
    for (const auto &slot : slots_) {
        if (live(slot, nowMs)) n++;
    }
    return n;
}

}  // namespace hubbo::auth
