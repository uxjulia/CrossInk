"""Check that volatile build identity reaches only BuildInfo.cpp."""
from pathlib import Path
import runpy
import unittest
import os
import re
import subprocess
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
MODULE = runpy.run_path(str(ROOT / "scripts/git_branch.py"))

class Env(dict):
    def __init__(self, name="default"):
        super().__init__(PROJECT_DIR=str(ROOT), PIOENV=name)
        self.defines = []
        self.middleware = []
    def Append(self, **kwargs): self.defines.extend(kwargs.get("CPPDEFINES", []))
    def Clone(self):
        copy = Env(self["PIOENV"])
        copy.defines = list(self.defines)
        return copy
    def Object(self, node): return (node, self.defines)
    def AddBuildMiddleware(self, callback, pattern): self.middleware.append((callback, pattern))

class BuildIdentityTest(unittest.TestCase):
    def test_asset_download_channel_overrides_version(self):
        versions = ("", "RC", "1.6.1", "1.6.1-x4-pro", "1.6.1-abc1234-RC",
                    "2.0.0-local-development+abc1234", "2.0.0-RC-local-development+abc1234")
        for channel in (None, "rc", "stable"):
            with self.subTest(channel=channel):
                source = '#include "AssetDownloadService.h"\n'
                for version in versions:
                    rc = channel == "rc" if channel is not None else version.endswith("-RC")
                    expected = "Rc" if rc else "Stable"
                    source += (f'static_assert(AssetDownloadService::channelForVersion("{version}") '
                               f'== AssetCatalog::Channel::{expected});\n')
                command = ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                           "-I", str(ROOT / "src/network"), "-I", str(ROOT / "lib/AssetCatalog"),
                           "-x", "c++", "-fsyntax-only", "-"]
                if channel is not None:
                    command.append(f'-DASSET_DOWNLOAD_CHANNEL="{channel}"')
                subprocess.run(command, input=source, text=True, check=True, capture_output=True)

    def test_asset_download_channel_rejects_invalid_override(self):
        for channel in ("", "RC", "beta"):
            with self.subTest(channel=channel):
                result = subprocess.run(
                    ["c++", "-std=c++17", "-I", str(ROOT / "src/network"),
                     "-I", str(ROOT / "lib/AssetCatalog"),
                     f'-DASSET_DOWNLOAD_CHANNEL="{channel}"', "-x", "c++", "-fsyntax-only", "-"],
                    input='#include "AssetDownloadService.h"\n', text=True, capture_output=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('ASSET_DOWNLOAD_CHANNEL must be "rc" or "stable"', result.stderr)

    def test_download_endpoint_is_explicit_and_validated(self):
        base = "https://assets.example.invalid/testing/abc/"
        function = MODULE["inject_download_endpoints"]
        env = Env()
        with patch.dict(os.environ, {"CROSSINK_ASSET_BASE_URL": base}, clear=True):
            function(env)
        self.assertEqual(len(env.defines), 1)
        self.assertEqual(env.defines[0][0], "ASSET_DOWNLOAD_BASE_URL")
        self.assertIn(base, env.defines[0][1])
        for invalid in ("http://host/", "https://host/no-slash", "https://host/../", "https://user:pass@host/", 'https://host/"bad/'):
            with patch.dict(os.environ, {"CROSSINK_ASSET_BASE_URL": invalid}, clear=True):
                with self.assertRaises(ValueError): function(Env())
        with patch.dict(os.environ, {"CROSSINK_ASSET_BASE_URL": ""}, clear=True):
            empty = Env(); function(empty)
            self.assertEqual(empty.defines, [])

    def test_font_endpoint_is_opt_in_and_validated(self):
        function = MODULE["inject_download_endpoints"]
        with patch.dict(os.environ, {}, clear=True):
            env = Env(); function(env)
            self.assertEqual(env.defines, [])
        for base in ("http://fonts.example.com/", "https://fonts.example.com/custom/"):
            with patch.dict(os.environ, {"CROSSINK_FONT_BASE_URL": base}, clear=True):
                env = Env(); function(env)
                self.assertEqual(len(env.defines), 1)
                self.assertEqual(env.defines[0][0], "FONT_DOWNLOAD_BASE_URL")
                self.assertIn(base, env.defines[0][1])
        for invalid in ("ftp://host/", "https://host/no-slash", "https://host/../",
                        "https://user:pass@host/", 'https://host/"bad/', "https://host/\n"):
            with patch.dict(os.environ, {"CROSSINK_FONT_BASE_URL": invalid}, clear=True):
                with self.assertRaises(ValueError): function(Env())
        with patch.dict(os.environ, {"CROSSINK_ASSET_BASE_URL": "https://assets.example.com/",
                                    "CROSSINK_FONT_BASE_URL": "http://fonts.example.com/"}, clear=True):
            env = Env(); function(env)
            self.assertEqual([item[0] for item in env.defines],
                             ["ASSET_DOWNLOAD_BASE_URL", "FONT_DOWNLOAD_BASE_URL"])

    def test_font_manifest_url_matches_build_configuration_and_format(self):
        header = (ROOT / "src/activities/settings/FontDownloadActivity.h").read_text()
        macros = header[header.index("#define FONTS_MANIFEST_VERSION"):header.index("class FontDownloadActivity")]
        font_header = (ROOT / "lib/EpdFont/SdCardFont.h").read_text()
        version = re.search(r"^#define CPFONT_VERSION ([0-9]+)$", font_header, re.M).group(1)
        manifest_version = re.search(r"^#define FONTS_MANIFEST_VERSION ([0-9]+)$", macros, re.M).group(1)
        for prefix in (None, "http://fonts.example.com/", "https://fonts.example.com/custom/"):
            with self.subTest(prefix=prefix):
                source = f"#define CPFONT_VERSION {version}\n"
                if prefix is not None:
                    source += f'#define FONT_DOWNLOAD_BASE_URL "{prefix}"\n'
                effective_prefix = prefix or "https://github.com/uxjulia/crossink-fonts/releases/download/"
                catalog_base = f"{effective_prefix}sd-fonts-m{manifest_version}-b{version}/"
                expected = catalog_base + "fonts.json"
                source += macros + f'\nstatic_assert(__builtin_strcmp(FONT_MANIFEST_URL, "{expected}") == 0);\n'
                source += f'static_assert(__builtin_strcmp(FONT_DOWNLOAD_CATALOG_BASE_URL, "{catalog_base}") == 0);\n'
                if prefix is None:
                    source += '#ifndef FONT_DOWNLOAD_USE_GITHUB_RELEASES\n#error Missing GitHub fallback\n#endif\n'
                else:
                    source += '#ifdef FONT_DOWNLOAD_USE_GITHUB_RELEASES\n#error Custom host must retain its catalog base URL\n#endif\n'
                subprocess.run(["c++", "-std=c++17", "-x", "c++", "-fsyntax-only", "-"],
                               input=source, text=True, check=True, capture_output=True)

    def test_identity_only_changes_one_compile_environment(self):
        function = MODULE["inject_version"]
        globals_ = function.__globals__
        previous = globals_["get_git_short_sha"]
        self.addCleanup(globals_.__setitem__, "get_git_short_sha", previous)
        for name in ("default", "sticky", "x4-pro", "x4-classic", "eego_a4", "eego_a4-debug", "debug", "sticky-debug", "x4-pro-debug", "test", "gh_release_rc", "x4-pro-simulator"):
            captures = []
            for sha in ("abc1234", "def5678"):
                globals_["get_git_short_sha"] = lambda _: sha
                env = Env(name)
                function(env)
                self.assertEqual(len(env.middleware), 1)
                callback, pattern = env.middleware[0]
                self.assertEqual(pattern, "*src/util/BuildInfo.cpp")
                _, scoped = callback(env, "BuildInfo.cpp")
                names = [d[0] for d in env.defines if isinstance(d, tuple)]
                self.assertNotIn("CROSSINK_GIT_SHA", names)
                self.assertNotIn("CROSSINK_GIT_DIRTY", names)
                self.assertNotIn("CROSSINK_VERSION", names)
                self.assertIn("CROSSINK_GIT_SHA", dict(d for d in scoped if isinstance(d, tuple)))
                captures.append((env.defines, scoped))
            self.assertEqual(captures[0][0], captures[1][0])
            self.assertNotEqual(captures[0][1], captures[1][1])

    def test_support_provenance_excludes_custom_version_and_invalid_sdk_values(self):
        function = MODULE["inject_version"]
        globals_ = function.__globals__
        for name in ("get_crossink_version", "run_git_value"):
            previous = globals_[name]
            self.addCleanup(globals_.__setitem__, name, previous)
        globals_["get_crossink_version"] = lambda _: "1.2.3+PRIVATE_BRANCH_PATH"
        globals_["run_git_value"] = lambda *_: "PRIVATE_SDK_PATH"
        env = Env("x4-pro-simulator")
        function(env)
        _, defines = env.middleware[0][0](env, "BuildInfo.cpp")
        values = dict(d for d in defines if isinstance(d, tuple))
        self.assertIn("unknown", values["CROSSINK_SUPPORT_VERSION"])
        self.assertIn("unknown", values["CROSSINK_SDK_SHA"])
        globals_["get_crossink_version"] = lambda _: "1.2.3"
        globals_["run_git_value"] = lambda *_: "a" * 40
        env = Env("x4-pro-simulator")
        function(env)
        _, defines = env.middleware[0][0](env, "BuildInfo.cpp")
        values = dict(d for d in defines if isinstance(d, tuple))
        self.assertIn("1.2.3", values["CROSSINK_SUPPORT_VERSION"])
        self.assertIn("a" * 40, values["CROSSINK_SDK_SHA"])

    def test_support_version_honors_release_override_and_rc_precedence(self):
        function = MODULE["inject_version"]
        globals_ = function.__globals__
        previous = globals_["get_crossink_version"]
        self.addCleanup(globals_.__setitem__, "get_crossink_version", previous)
        globals_["get_crossink_version"] = lambda _: "1.6.1"
        for release, rc, expected in (("1.6.2", "", "1.6.2"),
                                       ("v1.6.3", "", "1.6.3"),
                                       ("1.6.2", "abc123", "1.6.1"),
                                       ("PRIVATE_VERSION", "", "unknown")):
            with patch.dict(os.environ, {"CROSSINK_RELEASE_VERSION": release, "CROSSINK_RC_HASH": rc}):
                env = Env("default")
                function(env)
                _, defines = env.middleware[0][0](env, "BuildInfo.cpp")
                values = dict(d for d in defines if isinstance(d, tuple))
                self.assertIn(expected, values["CROSSINK_SUPPORT_VERSION"])

if __name__ == "__main__": unittest.main()
