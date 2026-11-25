#include <gtest/gtest.h>

#include "util/PluginLocations.h"

TEST(PluginManifestClassification, InstallingMarkerQuarantinesOtherwiseValidPlugins) {
  EXPECT_TRUE(PluginLocations::isDiscoverableEntry(false, false, true, false));
  EXPECT_TRUE(PluginLocations::isDiscoverableEntry(false, true, false, false));
  EXPECT_TRUE(PluginLocations::isDiscoverableEntry(false, false, false, true));
  EXPECT_FALSE(PluginLocations::isDiscoverableEntry(true, true, true, true));
  EXPECT_FALSE(PluginLocations::isDiscoverableEntry(false, false, false, false));
}

TEST(PluginManifestClassification, DirectPluginBundlesMayReuseInstalledRoot) {
  EXPECT_TRUE(PluginLocations::shouldReuseInstalledBundleDir("/.crosspoint/plugins", "foo"));
  EXPECT_TRUE(PluginLocations::shouldReuseInstalledBundleDir("/plugins", "foo"));
  EXPECT_TRUE(PluginLocations::shouldReuseInstalledBundleDir("/.plugins", "foo"));

  EXPECT_FALSE(PluginLocations::shouldReuseInstalledBundleDir("/downloads", "foo"));
  EXPECT_FALSE(PluginLocations::shouldReuseInstalledBundleDir("/.crosspoint/plugins", "foo/assets"));
  EXPECT_FALSE(PluginLocations::shouldReuseInstalledBundleDir("/.crosspoint/plugins", ""));
}

TEST(PluginManifestClassification, BrowseUrlIsCatalog) {
  EXPECT_EQ(PluginLocations::classifyDeviceManifest(true, false), PluginLocations::DeviceKind::Catalog);
  EXPECT_EQ(PluginLocations::classifyDeviceManifest(true, true), PluginLocations::DeviceKind::Catalog);
}

TEST(PluginManifestClassification, EventsWithoutBrowseAreBackgroundOnly) {
  EXPECT_EQ(PluginLocations::classifyDeviceManifest(false, true), PluginLocations::DeviceKind::Background);
}

TEST(PluginManifestClassification, EmptyDeviceManifestIsNotOpenable) {
  EXPECT_EQ(PluginLocations::classifyDeviceManifest(false, false), PluginLocations::DeviceKind::None);
}

TEST(PluginManifestClassification, OnlyANewerCatalogVersionIsAnUpdate) {
  EXPECT_TRUE(PluginLocations::isNewerVersion("1.1.0", "1.0.0"));
  EXPECT_TRUE(PluginLocations::isNewerVersion("0.1.10", "0.1.9"));
  EXPECT_TRUE(PluginLocations::isNewerVersion("2.0.0", "1.9.9"));
  EXPECT_FALSE(PluginLocations::isNewerVersion("1.0.0", "1.1.0"));  // no downgrade offers
  EXPECT_FALSE(PluginLocations::isNewerVersion("1.2.0", "1.2.0"));
  EXPECT_TRUE(PluginLocations::isNewerVersion("1.0.0", ""));  // installed copy without a version
  EXPECT_FALSE(PluginLocations::isNewerVersion("", "1.0.0"));
  EXPECT_FALSE(PluginLocations::isNewerVersion("1.2", "1.0.0"));  // catalog must be MAJOR.MINOR.PATCH
}

TEST(PluginManifestClassification, VersionsMustBeExactlyThreeDigitComponents) {
  for (const char* bad :
       {"1.2.3-beta", "1.2.3 ", "1.2.3.4", "-1.2.3", "+1.2.3", " 1.2.3", "1..3", "1.2.", "1.2.99999999999", "v1.2.3"}) {
    EXPECT_FALSE(PluginLocations::isNewerVersion(bad, "0.0.1")) << bad;  // never offered
    EXPECT_TRUE(PluginLocations::isNewerVersion("0.0.1", bad)) << bad;   // installed treated as unversioned
  }
  EXPECT_TRUE(PluginLocations::isNewerVersion("4294967295.0.0", "1.0.0"));  // UINT32_MAX still fits
}

TEST(PluginManifestClassification, RejectsTraversalAndFatAliases) {
  for (const auto* name : {"", ".", "..", "../wifi", "a/b", "a\\b", " space", "space ", "name.", "ABCDEF~1"})
    EXPECT_FALSE(PluginLocations::validName(name)) << name;
  EXPECT_TRUE(PluginLocations::validName("my-plugin"));
  EXPECT_FALSE(PluginLocations::validName(std::string(81, 'a')));
}
