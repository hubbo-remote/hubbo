#include "hubbo/remote/button.h"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <utility>

using namespace hubbo::remote;

TEST(Button, GroupMasksPartitionTheButtons) {
    const ButtonSet keypad = groupMask(kGroupKeypad);
    const ButtonSet media = groupMask(kGroupMediaHome);
    EXPECT_EQ(keypad & media, 0u);
    EXPECT_EQ(keypad | media, (ButtonSet{1} << kButtonCount) - 1);
    EXPECT_NE(keypad & buttonBit(Button::Up), 0u);
    EXPECT_NE(media & buttonBit(Button::Off), 0u);
    EXPECT_EQ(groupMask(0xC2), 0u);
}

TEST(Button, NamesAndCodesAreUnique) {
    std::set<std::string> names;
    std::set<std::pair<uint8_t, uint16_t>> codes;
    for (size_t i = 0; i < kButtonCount; i++) {
        const auto &info = buttonInfo(static_cast<Button>(i));
        EXPECT_TRUE(names.insert(info.name).second) << info.name;
        EXPECT_TRUE(codes.insert({info.group, info.code}).second) << info.name;
        EXPECT_NE(info.code, 0) << info.name;
    }
}

TEST(Button, NameRoundTrip) {
    for (size_t i = 0; i < kButtonCount; i++) {
        const auto button = static_cast<Button>(i);
        Button found;
        ASSERT_TRUE(buttonFromName(buttonName(button), &found));
        EXPECT_EQ(found, button);
    }
}

TEST(Button, UnknownLookupsFail) {
    Button found;
    EXPECT_FALSE(buttonFromName("ok", &found));
    EXPECT_FALSE(buttonFromName("", &found));
    EXPECT_FALSE(buttonFromName(nullptr, &found));
    EXPECT_FALSE(buttonFromCode(kGroupKeypad, 0x01EC, &found));  // off's code, wrong group
}

TEST(Button, KnownCodes) {
    Button found;
    ASSERT_TRUE(buttonFromCode(kGroupMediaHome, 0x01EC, &found));
    EXPECT_EQ(found, Button::Off);
    ASSERT_TRUE(buttonFromCode(kGroupKeypad, 0x0058, &found));
    EXPECT_EQ(found, Button::Select);
}
