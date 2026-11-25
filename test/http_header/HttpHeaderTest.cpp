#include <gtest/gtest.h>

#include "HttpHeader.h"
TEST(HttpHeader, ParseMaskClearAndRejectInjection) {
  HttpHeader header;
  ASSERT_TRUE(parseHttpHeaderLine(" X-Token: abc:def ", header));
  EXPECT_EQ(header.name, "X-Token");
  EXPECT_EQ(header.value, "abc:def");
  EXPECT_EQ(formatHttpHeaderLine(header, true), "X-Token: ******");
  EXPECT_FALSE(parseHttpHeaderLine("X-Token: x\r\nHost: evil", header));
  EXPECT_EQ(header.value, "abc:def");
  EXPECT_FALSE(parseHttpHeaderLine("Content-Length: 100", header));
  EXPECT_FALSE(parseHttpHeaderLine("Bad Name: abc", header));
  EXPECT_FALSE(isValidHttpHeader("X-Test", std::string(161, 'x')));
  EXPECT_TRUE(parseHttpHeaderLine("", header));
  EXPECT_TRUE(header.name.empty());
}
