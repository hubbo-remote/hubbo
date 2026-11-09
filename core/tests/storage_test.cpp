// Storage port: stdio and memory backends, file names, health. The record
// store built on it has its own suite (record_store_test.cpp).
#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <random>

#include "hubbo/storage/storage.h"

using namespace hubbo;
using namespace hubbo::storage;

TEST(Storage, FileNames) {
    EXPECT_TRUE(isValidFileName("config.json.tmp"));
    for (const char *bad : {"", ".", "..", "a/b", "../x", "a b", "x\\y"}) EXPECT_FALSE(isValidFileName(bad)) << bad;
}

TEST(Storage, StdioBackend) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("hubbo_storage_" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(dir);
    StdioStorage s(dir.string());
    std::string out;
    EXPECT_EQ(s.read("missing", &out, 100), ErrorCode::NotFound);
    ASSERT_EQ(s.write("a", "hello"), ErrorCode::Ok);
    ASSERT_EQ(s.read("a", &out, 100), ErrorCode::Ok);
    EXPECT_EQ(out, "hello");
    EXPECT_EQ(s.read("a", &out, 3), ErrorCode::TooLong);
    ASSERT_EQ(s.write("b", "old"), ErrorCode::Ok);
    ASSERT_EQ(s.write("b", "new"), ErrorCode::Ok);  // overwrites in place
    ASSERT_EQ(s.read("b", &out, 100), ErrorCode::Ok);
    EXPECT_EQ(out, "new");
    EXPECT_EQ(s.remove("b"), ErrorCode::Ok);
    EXPECT_EQ(s.remove("b"), ErrorCode::Ok);  // already gone is fine
    EXPECT_EQ(s.write("../escape", "x"), ErrorCode::StorageFailed);
    std::filesystem::remove_all(dir);
}

TEST(Storage, Health) {
    StdioStorage missing("/nonexistent/hubbo");
    EXPECT_EQ(missing.health(), Health::Unavailable);
    MemoryStorage mem;
    EXPECT_EQ(mem.health(), Health::Available);
    mem.setHealth(Health::ReadOnly);
    EXPECT_STREQ(healthName(mem.health()), "read_only");
}
