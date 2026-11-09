#include "hubbo/storage/state_store.h"

namespace hubbo::storage {

ErrorCode MemoryStateStore::get(std::string_view key, std::string *value) const {
    if (failReads) return ErrorCode::StorageFailed;
    const auto it = values_.find(key);
    if (it == values_.end()) return ErrorCode::NotFound;
    *value = it->second;
    return ErrorCode::Ok;
}

bool MemoryStateStore::set(std::string_view key, std::string_view value) {
    if (failWrites || !isValidKey(key) || value.size() > kMaxStateValueLength) return false;
    values_[std::string(key)] = std::string(value);
    writes_++;
    return true;
}

ErrorCode RecordStateStore::get(std::string_view key, std::string *value) const {
    if (!isValidKey(key)) return ErrorCode::NotFound;
    Record record;
    const ErrorCode loaded = store_.load(key, &record);
    if (loaded == ErrorCode::Ok) *value = std::move(record.payload);
    return loaded;
}

bool RecordStateStore::set(std::string_view key, std::string_view value) {
    if (!isValidKey(key) || value.size() > kMaxStateValueLength) return false;
    uint64_t generation = 0;
    if (store_.nextGeneration(key, &generation) != ErrorCode::Ok) return false;
    return store_.save(key, generation, value) == ErrorCode::Ok;
}

}  // namespace hubbo::storage
