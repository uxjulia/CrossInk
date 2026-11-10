"""Check the negotiated TLS record-size request in every firmware profile.

These are configuration guards, not a TLS handshake or hardware memory test.
Requires the PlatformIO CLI on PATH.
"""
import json
from pathlib import Path
import subprocess
import shutil
import unittest

ROOT = Path(__file__).resolve().parents[1]


class TlsFragmentConfigTest(unittest.TestCase):
    def test_firmware_profiles_request_without_forcing_small_records(self):
        pio = shutil.which("pio")
        self.assertIsNotNone(pio, "PlatformIO CLI is required for this configuration test")
        config = json.loads(subprocess.check_output(
            [pio, "project", "config", "--json-output"],
            cwd=ROOT, text=True,
        ))
        profiles = 0
        for section, values in config:
            if not section.startswith("env:"):
                continue
            flags = dict(values).get("build_flags", [])
            if "-DFREEINK_NET_WOLFSSL=1" not in flags:
                continue
            profiles += 1
            with self.subTest(profile=section):
                self.assertIn("-DHAVE_MAX_FRAGMENT", flags)
                # Peers that do not negotiate MFL must retain normal 16KB support.
                self.assertFalse(any("MAX_RECORD_SIZE" in flag for flag in flags))
        self.assertGreater(profiles, 0)

    def test_pinned_sdk_requests_2048_byte_fragments(self):
        source = (ROOT / "freeink-sdk/libs/network/SecureNet/src/SecureClient.cpp").read_text()
        self.assertIn("#ifdef HAVE_MAX_FRAGMENT", source)
        self.assertIn("wolfSSL_UseMaxFragment(ssl, WOLFSSL_MFL_2_11);", source)


if __name__ == "__main__":
    unittest.main()
