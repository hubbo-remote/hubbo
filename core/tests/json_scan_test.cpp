// The structure scanner the HA client uses to walk big discovery results.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "hubbo/integrations/json_scan.h"

using namespace hubbo::integrations;

TEST(JsonScan, FindsTopLevelMembersOnly) {
    const std::string text = R"({"id":7, "type" : "result","nested":{"success":false},"success":true,"result":[1,2]})";
    EXPECT_EQ(json::member(text, "id").in(text), "7");
    EXPECT_EQ(json::member(text, "type").in(text), "\"result\"");
    EXPECT_EQ(json::member(text, "success").in(text), "true");  // not the nested one
    EXPECT_EQ(json::member(text, "result").in(text), "[1,2]");
    EXPECT_FALSE(json::member(text, "missing").found());
    EXPECT_FALSE(json::member("[1]", "id").found());
}

TEST(JsonScan, StringsWithBracketsAndEscapesDontConfuseIt) {
    const std::string text = R"({"a":"x}]\"{[","b":[{"s":"]\\"},{"t":"[["}],"c":null})";
    EXPECT_EQ(json::member(text, "c").in(text), "null");
    std::vector<std::string> elements;
    json::Cursor cursor(text, json::valueStart(text, 0, "b"));
    std::string_view e;
    while (cursor.next(text, &e)) elements.emplace_back(e);
    EXPECT_TRUE(cursor.ok());
    EXPECT_EQ(elements, (std::vector<std::string>{R"({"s":"]\\"})", R"({"t":"[["})"}));
}

TEST(JsonScan, WalksArraysAndRejectsMalformedOnes) {
    auto walk = [](const std::string &text, bool *ok) {
        std::vector<std::string> out;
        json::Cursor cursor(text, text.find('['));
        std::string_view e;
        while (cursor.next(text, &e)) out.emplace_back(e);
        *ok = cursor.ok();
        return out;
    };
    bool ok = false;
    EXPECT_EQ(walk("[]", &ok), std::vector<std::string>{});
    EXPECT_TRUE(ok);
    EXPECT_EQ(walk(" [ 1 , \"two\" ,{\"x\":[3]} ] ", &ok), (std::vector<std::string>{"1", "\"two\"", "{\"x\":[3]}"}));
    EXPECT_TRUE(ok);
    walk("[1 2]", &ok);
    EXPECT_FALSE(ok);
    walk("[{\"a\":1]", &ok);
    EXPECT_FALSE(ok);
    walk("[\"unterminated]", &ok);
    EXPECT_FALSE(ok);
    EXPECT_FALSE(json::Cursor("12", 0).ok());  // not an array or object
}

TEST(JsonScan, WalksObjectMembersWithTheirKeys) {
    // HA's compressed states (subscribe_entities): entity id -> state.
    const std::string text = R"({"id":3,"event":{"a":{"light.x":{"s":"on","a":{"k":"}"}}, "switch.y" : {"s":"off"}}}})";
    const json::Span a = json::member(text, json::member(text, "event"), "a");
    ASSERT_TRUE(a.found());
    json::Cursor cursor(text, a.begin);
    std::string_view key, value;
    std::vector<std::string> keys, values;
    while (cursor.next(text, &value, &key)) {
        keys.emplace_back(key);
        values.emplace_back(value);
    }
    EXPECT_TRUE(cursor.ok());
    EXPECT_EQ(keys, (std::vector<std::string>{"light.x", "switch.y"}));
    EXPECT_EQ(values, (std::vector<std::string>{R"({"s":"on","a":{"k":"}"}})", R"({"s":"off"})"}));
    EXPECT_EQ(json::member(text, a, "switch.y").in(text), R"({"s":"off"})");
    // The way in without scanning the big value: where it starts.
    EXPECT_EQ(json::valueStart(text, json::valueStart(text, 0, "event"), "a"), a.begin);
    EXPECT_EQ(json::valueStart(text, 0, "missing"), json::npos);

    const std::string bad = R"({"k" 1})";
    json::Cursor broken(bad, 0);
    EXPECT_FALSE(broken.next(bad, &value, &key));
    EXPECT_FALSE(broken.ok());
}
