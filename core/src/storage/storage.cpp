#include "hubbo/storage/storage.h"

#include <cerrno>
#include <cstdio>

#if __has_include(<unistd.h>)
#include <unistd.h>
#define HUBBO_HAVE_FSYNC 1
#endif
#if __has_include(<sys/stat.h>)
#include <sys/stat.h>
#define HUBBO_HAVE_STAT 1
#endif

namespace hubbo::storage {

const char *healthName(Health health) {
    switch (health) {
        case Health::Available: return "available";
        case Health::Unavailable: return "unavailable";
        case Health::ReadOnly: return "read_only";
    }
    return "unavailable";
}

bool isValidKey(std::string_view key) {
    if (key.empty() || key.size() > kMaxKeyLength) return false;
    for (char c : key) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

bool isValidFileName(std::string_view name) {
    if (name.empty() || name.size() > 64 || name == "." || name == "..") return false;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

StdioStorage::StdioStorage(std::string root) : root_(std::move(root)) {}

Health StdioStorage::health() const {
#ifdef HUBBO_HAVE_STAT
    struct stat st {};
    if (::stat(root_.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return Health::Unavailable;
#endif
#ifdef HUBBO_HAVE_FSYNC
    if (::access(root_.c_str(), W_OK) != 0) return Health::ReadOnly;
#endif
    return Health::Available;
}

ErrorCode StdioStorage::read(const std::string &name, std::string *out, size_t maxBytes) {
    if (!isValidFileName(name)) return ErrorCode::StorageFailed;
    std::FILE *f = std::fopen(path(name).c_str(), "rb");
    if (f == nullptr) return errno == ENOENT ? ErrorCode::NotFound : ErrorCode::StorageFailed;
    std::string data;
    char buf[1024];
    size_t n;
    ErrorCode result = ErrorCode::Ok;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        if (data.size() + n > maxBytes) {
            result = ErrorCode::TooLong;
            break;
        }
        data.append(buf, n);
    }
    if (result == ErrorCode::Ok && std::ferror(f)) result = ErrorCode::StorageFailed;
    std::fclose(f);
    if (result == ErrorCode::Ok) *out = std::move(data);
    return result;
}

ErrorCode StdioStorage::write(const std::string &name, std::string_view data) {
    if (!isValidFileName(name)) return ErrorCode::StorageFailed;
    std::FILE *f = std::fopen(path(name).c_str(), "wb");
    if (f == nullptr) return ErrorCode::StorageFailed;
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = std::fflush(f) == 0 && ok;
#ifdef HUBBO_HAVE_FSYNC
    ok = ::fsync(fileno(f)) == 0 && ok;
#endif
    ok = std::fclose(f) == 0 && ok;
    return ok ? ErrorCode::Ok : ErrorCode::StorageFailed;
}

ErrorCode StdioStorage::remove(const std::string &name) {
    if (!isValidFileName(name)) return ErrorCode::StorageFailed;
    if (std::remove(path(name).c_str()) == 0 || errno == ENOENT) return ErrorCode::Ok;
    return ErrorCode::StorageFailed;
}

bool MemoryStorage::powerLost() {
    if (crashed_) return true;
    if (crashAfter_ == 0) {
        crashed_ = true;
        return true;
    }
    if (crashAfter_ > 0) crashAfter_--;
    mutations_++;
    return false;
}

ErrorCode MemoryStorage::read(const std::string &name, std::string *out, size_t maxBytes) {
    reads_++;
    if (failingReads_.count(name) != 0) return ErrorCode::StorageFailed;
    const auto it = files_.find(name);
    if (it == files_.end()) return ErrorCode::NotFound;
    if (it->second.size() > maxBytes) return ErrorCode::TooLong;
    *out = it->second;
    return ErrorCode::Ok;
}

ErrorCode MemoryStorage::write(const std::string &name, std::string_view data) {
    if (powerLost()) {
        if (crashed_ && tornBytes_ > 0) {
            files_[name] = std::string(data.substr(0, tornBytes_));
            tornBytes_ = 0;
        }
        return ErrorCode::StorageFailed;
    }
    files_[name] = std::string(data);
    return ErrorCode::Ok;
}

ErrorCode MemoryStorage::remove(const std::string &name) {
    if (powerLost()) return ErrorCode::StorageFailed;
    files_.erase(name);
    return ErrorCode::Ok;
}

}  // namespace hubbo::storage
