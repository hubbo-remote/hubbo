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
    json::ArrayCursor cursor(text, json::member(text, "b"));
    std::string_view e;
    while (cursor.next(text, &e)) elements.emplace_back(e);
    EXPECT_TRUE(cursor.ok());
    EXPECT_EQ(elements, (std::vector<std::string>{R"({"s":"]\\"})", R"({"t":"[["})"}));
}

TEST(JsonScan, WalksArraysAndRejectsMalformedOnes) {
    auto walk = [](const std::string &text, bool *ok) {
        std::vector<std::string> out;
        json::ArrayCursor cursor(text, json::Span{text.find('['), text.rfind(']') + 1});
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
    EXPECT_FALSE(json::ArrayCursor("{}", json::Span{0, 2}).ok());  // not an array
}
