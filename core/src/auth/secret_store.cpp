#include "hubbo/auth/secret_store.h"

#include <ArduinoJson.h>

namespace hubbo::auth {


const char *secretReadName(SecretRead result) {
    switch (result) {
        case SecretRead::Ok: return "ok";
        case SecretRead::NotFound: return "not_found";
        case SecretRead::Unavailable: return "unavailable";
        case SecretRead::Corrupt: return "corrupt";
    }
    return "unavailable";
}

// ---------------------------------------------------------------- memory

SecretRead MemorySecretStore::read(std::string_view key, std::string *value) const {
    if (healthState != storage::Health::Available && healthState != storage::Health::ReadOnly) {
        return SecretRead::Unavailable;
    }
    const auto it = values_.find(key);
    if (it == values_.end()) return SecretRead::NotFound;
    *value = it->second;
    return SecretRead::Ok;
}

bool MemorySecretStore::write(std::string_view key, std::string_view value) {
    if (failWrites || healthState != storage::Health::Available || !storage::isValidKey(key) ||
        value.size() > kMaxSecretValueLength) {
        return false;
    }
    values_.insert_or_assign(std::string(key), std::string(value));
    return true;
}

bool MemorySecretStore::erase(std::string_view key) {
    if (failWrites || healthState != storage::Health::Available) return false;
    const auto it = values_.find(key);
    if (it != values_.end()) values_.erase(it);
    return true;
}

SecretRead MemorySecretStore::keys(std::string_view prefix, std::vector<std::string> *out) const {
    if (healthState != storage::Health::Available && healthState != storage::Health::ReadOnly) {
        return SecretRead::Unavailable;
    }
    out->clear();
    for (const auto &entry : values_) {
        if (std::string_view(entry.first).substr(0, prefix.size()) == prefix) out->push_back(entry.first);
    }
    return SecretRead::Ok;
}

bool eraseAllSecrets(SecretStore &store) {
    std::vector<std::string> keys;
    if (store.keys("", &keys) != SecretRead::Ok) return false;
    bool ok = true;
    for (const auto &key : keys) ok = store.erase(key) && ok;
    return ok;
}

// ---------------------------------------------------------------- record store

StorageSecretStore::StorageSecretStore(storage::RecordStore &records) : records_(records) { reload(); }

void StorageSecretStore::reload() {
    memory_ = MemorySecretStore();
    if (records_.health() != storage::Health::Available && records_.health() != storage::Health::ReadOnly) {
        loaded_ = SecretRead::Unavailable;
        return;
    }
    storage::Record record;
    switch (records_.load(kRecordName, &record)) {
        case ErrorCode::Ok: break;
        case ErrorCode::NotFound: loaded_ = SecretRead::NotFound; return;  // first boot: no secrets yet
        case ErrorCode::Corrupt: loaded_ = SecretRead::Corrupt; return;    // present, no intact copy: never absent
        default: loaded_ = SecretRead::Unavailable; return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, record.payload) || !doc.is<JsonObjectConst>()) {
        loaded_ = SecretRead::Corrupt;
        return;
    }
    for (JsonPairConst pair : doc.as<JsonObjectConst>()) {
        if (!pair.value().is<const char *>() || !memory_.write(pair.key().c_str(), pair.value().as<const char *>())) {
            memory_ = MemorySecretStore();
            loaded_ = SecretRead::Corrupt;
            return;
        }
    }
    loaded_ = SecretRead::Ok;
}

storage::Health StorageSecretStore::health() const {
    if (loaded_ == SecretRead::Unavailable) return storage::Health::Unavailable;
    return records_.health();
}

SecretRead StorageSecretStore::read(std::string_view key, std::string *value) const {
    if (!writable()) return loaded_;
    return memory_.read(key, value);
}

SecretRead StorageSecretStore::keys(std::string_view prefix, std::vector<std::string> *out) const {
    if (!writable()) return loaded_;
    return memory_.keys(prefix, out);
}

bool StorageSecretStore::save() {
    JsonDocument doc;
    JsonObject root = doc.to<JsonObject>();
    std::vector<std::string> all;
    memory_.keys("", &all);
    for (const auto &key : all) {
        std::string value;
        memory_.read(key, &value);
        root[key] = value;
    }
    std::string text;
    serializeJson(doc, text);
    // The store owns generations; and nothing is written to a store that
    // isn't fully trusted (ReadOnly means a slot couldn't be invalidated).
    if (records_.health() != storage::Health::Available) return false;
    uint64_t generation = 0;
    if (records_.nextGeneration(kRecordName, &generation) != ErrorCode::Ok) return false;
    if (records_.save(kRecordName, generation, text) != ErrorCode::Ok) return false;
    loaded_ = SecretRead::Ok;
    return true;
}

bool StorageSecretStore::write(std::string_view key, std::string_view value) {
    if (!writable()) return false;  // never overwrite secrets we couldn't read
    std::string previous;
    const bool had = memory_.read(key, &previous) == SecretRead::Ok;
    if (!memory_.write(key, value)) return false;
    if (save()) return true;
    if (had) {
        memory_.write(key, previous);
    } else {
        memory_.erase(key);
    }
    return false;
}

bool StorageSecretStore::erase(std::string_view key) {
    if (!writable()) return false;
    std::string previous;
    if (memory_.read(key, &previous) != SecretRead::Ok) return true;
    memory_.erase(key);
    if (save()) return true;
    memory_.write(key, previous);
    return false;
}

}  // namespace hubbo::auth
