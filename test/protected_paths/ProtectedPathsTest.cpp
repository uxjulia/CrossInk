#include <gtest/gtest.h>

#include "network/ProtectedPaths.h"

using protectedpaths::isPluginPath;
using protectedpaths::isSensitivePath;

TEST(ProtectedPaths, CredentialStoresAreSensitive) {
  EXPECT_TRUE(isSensitivePath("/.crosspoint/wifi.json"));
  EXPECT_TRUE(isSensitivePath("/.crosspoint/opds.json"));
  EXPECT_TRUE(isSensitivePath("/.crosspoint/koreader.json"));
  EXPECT_TRUE(isSensitivePath("/.crosspoint/wifi.json.tmp"));
  EXPECT_TRUE(isSensitivePath("/.CrossPoint/WIFI.json"));
}

TEST(ProtectedPaths, SpellingVariantsCannotDodgeTheMatch) {
  EXPECT_TRUE(isSensitivePath("/.crosspoint/./wifi.json"));
  EXPECT_TRUE(isSensitivePath("//.crosspoint//wifi.json"));
  EXPECT_TRUE(isSensitivePath("/.crosspoint/plugins/../wifi.json"));
  EXPECT_TRUE(isSensitivePath(".crosspoint/opds.json"));
}

TEST(ProtectedPaths, NamesSdFatWouldTrimOrAliasStillMatch) {
  EXPECT_TRUE(isSensitivePath("/.crosspoint/wifi.json."));
  EXPECT_TRUE(isSensitivePath("/.crosspoint./wifi.json"));
  EXPECT_TRUE(isSensitivePath("/ .crosspoint/ wifi.json"));
  EXPECT_TRUE(isSensitivePath("/.crosspoint ./opds.json"));
  EXPECT_TRUE(isSensitivePath("/CROSSP~1/wifi.json"));
  EXPECT_TRUE(isSensitivePath("/.crosspoint/WIFI~1.JSO"));
  EXPECT_FALSE(isSensitivePath("/Books/Photos/IMG~1.JPG"));  // too deep to reach a store
  EXPECT_FALSE(isSensitivePath("/Books/a~b.epub"));
}

TEST(ProtectedPaths, EverythingElseStaysReachable) {
  EXPECT_FALSE(isSensitivePath("/.crosspoint/recent.json"));
  EXPECT_FALSE(isSensitivePath("/.crosspoint/library.idx"));
  EXPECT_FALSE(isSensitivePath("/.crosspoint/month-wallpaper.json"));
  EXPECT_FALSE(isSensitivePath("/.fonts/Noto/Noto_14.cpfont"));
  EXPECT_FALSE(isSensitivePath("/Books/wifi.json"));
}

TEST(ProtectedPaths, PluginPathRules) {
  EXPECT_FALSE(isPluginPath("/.crosspoint/plugins/../wifi.json"));
  EXPECT_FALSE(isPluginPath("/.crosspoint/./koreader.json"));
  EXPECT_FALSE(isPluginPath("relative/path"));
  EXPECT_FALSE(isPluginPath("/"));
  EXPECT_TRUE(isPluginPath("/.crosspoint/month-wallpaper.json"));
  EXPECT_TRUE(isPluginPath("/Books/new.epub"));
}

TEST(ProtectedPaths, LegacyStoresAndDirectoryMutationsStayProtected) {
  EXPECT_TRUE(isSensitivePath("/.crosspoint/wifi.bin.bak"));
  EXPECT_TRUE(isSensitivePath("/.crosspoint/plugin-install.txt.part"));
  EXPECT_FALSE(isSensitivePath("/.crosspoint"));  // still browse other hidden files
  EXPECT_TRUE(isSensitivePath("/.crosspoint", true));
  EXPECT_TRUE(isSensitivePath("/.crosspoint/.", true));
  EXPECT_FALSE(isSensitivePath("/.crosspoint/plugins", true));
  EXPECT_FALSE(isPluginPath(std::string("/books/new.epub\0/other", 22)));
  EXPECT_FALSE(isPluginPath("/books\\..\\.crosspoint\\wifi.json"));
}

TEST(ProtectedPaths, LegacySettingsAndHashedFatAliasesStayProtected) {
  for (const char* name : {"/.crosspoint/settings.json", "/.crosspoint/settings.json.bak",
                           "/.crosspoint/settings.bin.tmp", "/.CrossPoint/crossink-settings.json.davold",
                           "/CRO~A123/wifi.json", "/cro~fabc/opds.json", "/.crosspoint/SET~ABCD.JSO"}) {
    EXPECT_TRUE(isSensitivePath(name)) << name;
    EXPECT_FALSE(isPluginPath(name)) << name;
  }
  EXPECT_FALSE(isSensitivePath("/Books/a~b.epub"));
  EXPECT_FALSE(isSensitivePath("/Books/Photos/CRO~A123.JPG"));
}
