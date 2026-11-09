#include "nvs_secret_store.h"

#include <nvs.h>

#include "hubbo/storage/storage.h"

namespace hubbo::fw {

using hubbo::auth::SecretRead;

namespace {

// An open handle on the namespace that closes itself. `error` keeps why
// opening failed: ESP_ERR_NVS_NOT_FOUND means the namespace doesn't exist yet
// (read-only open on a fresh hub), anything else is a real failure.
class Handle {
public:
    explicit Handle(nvs_open_mode_t mode) : error_(nvs_open(NvsSecretStore::kNamespace, mode, &handle_)) {}
    ~Handle() {
        if (error_ == ESP_OK) {
            nvs_close(handle_);
        }
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;

    esp_err_t error() const { return error_; }
    nvs_handle_t get() const { return handle_; }

private:
    nvs_handle_t handle_ = 0;
    esp_err_t error_;
};

SecretRead classify(esp_err_t err) {
    switch (err) {
        case ESP_OK: return SecretRead::Ok;
        case ESP_ERR_NVS_NOT_FOUND: return SecretRead::NotFound;
        case ESP_ERR_NVS_TYPE_MISMATCH: return SecretRead::Corrupt;  // the key holds something else
        default: return SecretRead::Unavailable;
    }
}

bool validValue(std::string_view value) {
    return value.size() <= hubbo::auth::kMaxSecretValueLength && value.find('\0') == std::string_view::npos;
}

}  // namespace

hubbo::storage::Health NvsSecretStore::health() const {
    const Handle handle(NVS_READONLY);
    const bool usable = handle.error() == ESP_OK || handle.error() == ESP_ERR_NVS_NOT_FOUND;
    return usable ? hubbo::storage::Health::Available : hubbo::storage::Health::Unavailable;
}

SecretRead NvsSecretStore::read(std::string_view key, std::string *value) const {
    if (!hubbo::storage::isValidKey(key)) {
        return SecretRead::NotFound;  // no such key can exist
    }
    const Handle handle(NVS_READONLY);
    if (handle.error() != ESP_OK) {
        return classify(handle.error());  // NOT_FOUND: namespace never created
    }
    const std::string name(key);
    size_t length = 0;
    const SecretRead sized = classify(nvs_get_str(handle.get(), name.c_str(), nullptr, &length));
    if (sized != SecretRead::Ok) {
        return sized;
    }
    if (length == 0) {
        return SecretRead::Corrupt;  // a string always has its NUL
    }
    std::string text(length, '\0');
    const SecretRead got = classify(nvs_get_str(handle.get(), name.c_str(), text.data(), &length));
    if (got != SecretRead::Ok) {
        return got == SecretRead::NotFound ? SecretRead::Unavailable : got;  // vanished mid-read
    }
    text.resize(length - 1);
    *value = std::move(text);
    return SecretRead::Ok;
}

bool NvsSecretStore::write(std::string_view key, std::string_view value) {
    if (!hubbo::storage::isValidKey(key) || !validValue(value)) {
        return false;
    }
    const std::string name(key);
    const std::string text(value);
    Handle handle(NVS_READWRITE);
    return handle.error() == ESP_OK && nvs_set_str(handle.get(), name.c_str(), text.c_str()) == ESP_OK &&
           nvs_commit(handle.get()) == ESP_OK;
}

bool NvsSecretStore::erase(std::string_view key) {
    if (!hubbo::storage::isValidKey(key)) {
        return false;
    }
    const std::string name(key);
    Handle handle(NVS_READWRITE);
    if (handle.error() != ESP_OK) {
        return false;
    }
    const esp_err_t err = nvs_erase_key(handle.get(), name.c_str());
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }
    return err == ESP_OK && nvs_commit(handle.get()) == ESP_OK;
}

SecretRead NvsSecretStore::keys(std::string_view prefix, std::vector<std::string> *out) const {
    std::vector<std::string> found;
    nvs_iterator_t it = nullptr;
    // NOT_FOUND here means no entries at all (or no namespace yet).
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, kNamespace, NVS_TYPE_STR, &it);
    while (err == ESP_OK) {
        nvs_entry_info_t info;
        if (nvs_entry_info(it, &info) != ESP_OK) {
            nvs_release_iterator(it);
            return SecretRead::Unavailable;
        }
        const std::string_view key(info.key);
        if (key.substr(0, prefix.size()) == prefix) {
            found.emplace_back(key);
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        return SecretRead::Unavailable;
    }
    *out = std::move(found);
    return SecretRead::Ok;
}

}  // namespace hubbo::fw
