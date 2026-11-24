#include "hubbo/remote/input.h"

#include <cstring>

#include "hubbo/remote/log.h"

namespace hubbo::remote {

namespace {

constexpr const char *kEventKindNames[] = {"down", "up", "tap", "hold"};
static_assert(sizeof(kEventKindNames) / sizeof(kEventKindNames[0]) == kEventKindCount,
              "kEventKindNames must name every EventKind");

// Iterates set bits lowest first, so events come out in Button order.
template <typename F>
void forEach(ButtonSet set, F &&fn) {
    while (set != 0) {
        const auto index = static_cast<uint8_t>(__builtin_ctzll(set));
        set &= set - 1;
        fn(static_cast<Button>(index));
    }
}

}  // namespace

const char *eventKindName(EventKind kind) {
    return kEventKindNames[static_cast<size_t>(kind)];
}

bool eventKindFromName(const char *name, EventKind *out) {
    if (name == nullptr) {
        return false;
    }
    for (size_t i = 0; i < kEventKindCount; i++) {
        if (std::strcmp(kEventKindNames[i], name) == 0) {
            *out = static_cast<EventKind>(i);
            return true;
        }
    }
    return false;
}

RemoteInput::RemoteInput(EventSink &sink, InputConfig config) : sink_(sink), config_(config) {}

void RemoteInput::onPayload(const uint8_t *bytes, size_t length, uint32_t nowMs, Signal signal) {
    poll(nowMs);
    lastPacketMs_ = nowMs;

    DecodedPacket packet;
    const DecodeResult decoded = decodePacket(bytes, length, &packet);
    logPayload(log_, nowMs, decoded, packet, held_, signal);
    switch (decoded) {
        case DecodeResult::Buttons:
            unknownCodes_ += packet.unknownCodes;
            applyGroup(packet.group, packet.held, nowMs);
            break;
        case DecodeResult::NotButtons:
            break;
        case DecodeResult::BadChecksum:
            badChecksums_++;
            break;
    }
}

void RemoteInput::poll(uint32_t nowMs) {
    if (held_ == 0) {
        return;
    }

    // If the remote has gone quiet, the release happened somewhere in the
    // silence. Judge holds as of the timeout, not as of a late poll.
    const bool timedOut = nowMs - lastPacketMs_ >= config_.releaseTimeoutMs;
    const uint32_t asOfMs = timedOut ? lastPacketMs_ + config_.releaseTimeoutMs : nowMs;

    forEach(held_ & ~holdFired_, [&](Button button) {
        const uint32_t downAt = downAtMs_[static_cast<size_t>(button)];
        if (asOfMs - downAt >= config_.holdMs) {
            holdFired_ |= buttonBit(button);
            emit(button, EventKind::Hold, downAt + config_.holdMs);
        }
    });

    if (timedOut) {
        releaseHeld(asOfMs, true);
    }
}

void RemoteInput::releaseAll(uint32_t nowMs) {
    releaseHeld(nowMs, false);
}

void RemoteInput::releaseHeld(uint32_t atMs, bool timedOut) {
    const ButtonSet released = held_;
    held_ = 0;
    forEach(released, [&](Button button) { release(button, atMs, timedOut); });
}

// A button payload is the complete held set for its group only; buttons in
// the other group are untouched.
void RemoteInput::applyGroup(uint8_t group, ButtonSet now, uint32_t nowMs) {
    const ButtonSet mask = groupMask(group);
    const ButtonSet released = held_ & mask & ~now;
    const ButtonSet pressed = now & ~held_;
    held_ = (held_ & ~mask) | now;
    // Releases first: a chord change from A to B reads as Up A, Down B.
    forEach(released, [&](Button button) { release(button, nowMs, false); });
    forEach(pressed, [&](Button button) { press(button, nowMs); });
}

void RemoteInput::press(Button button, uint32_t nowMs) {
    downAtMs_[static_cast<size_t>(button)] = nowMs;
    holdFired_ &= ~buttonBit(button);
    emit(button, EventKind::Down, nowMs);
}

void RemoteInput::release(Button button, uint32_t nowMs, bool timedOut) {
    emit(button, EventKind::Up, nowMs, timedOut);
    if ((holdFired_ & buttonBit(button)) == 0) {
        emit(button, EventKind::Tap, nowMs, timedOut);
    }
    holdFired_ &= ~buttonBit(button);
}

void RemoteInput::emit(Button button, EventKind kind, uint32_t atMs, bool timedOut) {
    RemoteEvent event{button, kind, atMs, timedOut};
    logEvent(log_, event);
    sink_.onRemoteEvent(event);
}

}  // namespace hubbo::remote
