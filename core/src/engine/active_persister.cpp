#include "hubbo/engine/active_persister.h"

#include "hubbo/domain/time.h"

namespace hubbo {

ActivePersister::ActivePersister(storage::StateStore &store, uint32_t delayMs) : store_(store), delayMs_(delayMs) {}

ErrorCode ActivePersister::savedActivity(std::string *out) {
    std::string value;
    const ErrorCode read = store_.get(kKey, &value);
    savedKnown_ = read == ErrorCode::Ok || read == ErrorCode::NotFound;  // else unreadable: unknown
    saved_ = read == ErrorCode::Ok ? value : std::string();
    out->assign(saved_);
    return read;
}

void ActivePersister::holdUntilChanged(std::string_view current) {
    started_ = true;
    observed_.assign(current);
    holding_ = true;
}

bool ActivePersister::pending() const {
    return started_ && !holding_ && (!savedKnown_ || observed_ != saved_);
}

void ActivePersister::observe(std::string_view current, uint32_t nowMs) {
    if (started_ && current == observed_) return;
    if (started_) holding_ = false;  // a real change ends the hold
    started_ = true;
    observed_.assign(current);
    changedAtMs_ = nowMs;
}

bool ActivePersister::write() {
    if (!store_.set(kKey, observed_)) return false;
    saved_ = observed_;
    savedKnown_ = true;
    writes_++;
    return true;
}

void ActivePersister::tick(std::string_view current, uint32_t nowMs, bool configOk) {
    observe(current, nowMs);
    if (!configOk || !pending() || !timeReached(nowMs, changedAtMs_ + delayMs_)) return;
    if (!write()) changedAtMs_ = nowMs;  // retry after another delay
}

void ActivePersister::flush(std::string_view current, bool configOk) {
    observe(current, changedAtMs_);
    if (configOk && pending()) write();
}

}  // namespace hubbo
