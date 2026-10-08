#include <gtest/gtest.h>

#include <array>

#include "controllers/midi/libremidimessage.h"

namespace {

TEST(LibremidiShortMessageTest, ThreeByteMessage) {
    const std::array<unsigned char, 3> bytes = {0x90, 0x3C, 0x7F};
    LibremidiShortMessage m{};
    ASSERT_TRUE(libremidiUnpackShortMessage(bytes, &m));
    EXPECT_EQ(0x90, m.status);
    EXPECT_EQ(0x3C, m.byte1);
    EXPECT_EQ(0x7F, m.byte2);
}

TEST(LibremidiShortMessageTest, TwoByteMessageDoesNotReadPastEnd) {
    // Program change: status + one data byte.
    const std::array<unsigned char, 2> bytes = {0xC1, 0x05};
    LibremidiShortMessage m{0xFF, 0xFF, 0xFF};
    ASSERT_TRUE(libremidiUnpackShortMessage(bytes, &m));
    EXPECT_EQ(0xC1, m.status);
    EXPECT_EQ(0x05, m.byte1);
    EXPECT_EQ(0, m.byte2);
}

TEST(LibremidiShortMessageTest, OneByteMessageDoesNotReadPastEnd) {
    const std::array<unsigned char, 1> bytes = {0xF6}; // tune request
    LibremidiShortMessage m{0xFF, 0xFF, 0xFF};
    ASSERT_TRUE(libremidiUnpackShortMessage(bytes, &m));
    EXPECT_EQ(0xF6, m.status);
    EXPECT_EQ(0, m.byte1);
    EXPECT_EQ(0, m.byte2);
}

TEST(LibremidiShortMessageTest, EmptyMessageIsRejected) {
    LibremidiShortMessage m{0xAA, 0xBB, 0xCC};
    EXPECT_FALSE(libremidiUnpackShortMessage(std::span<const unsigned char>{}, &m));
    // untouched
    EXPECT_EQ(0xAA, m.status);
}

TEST(LibremidiShortMessageTest, OnlyFirstThreeBytesAreUsed) {
    const std::array<unsigned char, 5> bytes = {0xB0, 0x01, 0x02, 0x03, 0x04};
    LibremidiShortMessage m{};
    ASSERT_TRUE(libremidiUnpackShortMessage(bytes, &m));
    EXPECT_EQ(0xB0, m.status);
    EXPECT_EQ(0x01, m.byte1);
    EXPECT_EQ(0x02, m.byte2);
}

} // namespace
