"""Check RSA dispatch against the installed wolfSSL source, without a device.

Set WOLFSSL_RSA_SOURCE when Arduino-wolfSSL lives outside this checkout.
The arithmetic is stubbed; this tests selection of SP versus general math.
"""
from pathlib import Path
import os
import runpy
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <assert.h>
#include <stddef.h>
typedef unsigned char byte;
typedef unsigned int word32;
typedef struct { int n, e; } RsaKey;
typedef int WC_RNG;
#define WC_KEY_SIZE_E -1
#define BAD_FUNC_ARG -2
#define RSA_BUFFER_E -3
#define MP_VAL -4
#define RSA_PUBLIC_ENCRYPT 0
#define RSA_PUBLIC_DECRYPT 1
#define RSA_PRIVATE_ENCRYPT 2
#define RSA_PRIVATE_DECRYPT 3
#define WOLFSSL_MSG(x) ((void)0)
#define WOLFSSL_MSG_EX(...) ((void)0)
#define WC_NO_ERR_TRACE(x) (x)
static int mp_count_bits(const int *n) { return *n; }
static int mp_iseven(const int *n) { (void)n; return 0; }
static int wc_RsaEncryptSize(RsaKey *key) { return key->n / 8; }
/* Distinct results make the selected arithmetic path observable. */
static int sp_result(int type, RsaKey *key) {
    if (key->n != 2048 && key->n != 3072 && key->n != 4096)
        return WC_KEY_SIZE_E;
    if ((type == RSA_PUBLIC_ENCRYPT || type == RSA_PUBLIC_DECRYPT) && key->e > 64)
        return -99;
    return 100;
}
#define RsaFunction_SP(i, il, o, ol, t, k, r) sp_result(t, k)
#define RsaFunctionSync(i, il, o, ol, t, k, r) ((void)i, (void)o, (void)r, 200)
'''
CHECKS = r'''
int main(void) {
    RsaKey key = {2048, 17};
    word32 len = 512;
    for (key.n = 2048; key.n <= 4096; key.n += 1024) {
        for (int type = 0; type < 4; ++type) {
            for (key.e = 64; key.e <= 65; ++key.e) {
                len = 512;
                int expected = 100;
                if (type < 2 && key.e > 64) {
#ifdef FREEINK_WOLFSSL_SP_RSA_FALLBACK
                    expected = 200;
#else
                    expected = -99;
#endif
                }
                assert(wc_RsaFunctionSync(NULL, 128, NULL, &len, type, &key, NULL) == expected);
            }
        }
    }
    key.n = 1024; key.e = 17; len = 512;
    assert(wc_RsaFunctionSync(NULL, 128, NULL, &len, RSA_PRIVATE_DECRYPT, &key, NULL) == 200);
    return 0;
}
'''


class RsaFallbackTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

        class Env:
            def subst(inner, value): return str(self.root)
            def Append(inner, **kwargs): pass

        self.module = runpy.run_path(str(ROOT / "scripts/patch_wolfssl.py"),
                                    init_globals={"Import": lambda _: None, "env": Env()})

    def test_idempotent_and_rejects_unknown_or_corrupt_source(self):
        path = self.root / "rsa.c"
        path.write_text(self.module["RSA_ORIGINAL"])
        patch = self.module["patch_rsa_fallback"]
        patch(path)
        once = path.read_bytes()
        patch(path)
        self.assertEqual(once, path.read_bytes())
        for invalid in ("unknown source", self.module["RSA_MARKER"]):
            path.write_text(invalid)
            with self.assertRaises(RuntimeError): patch(path)
            self.assertEqual(path.read_text(), invalid)

    def test_actual_dispatch_preserves_wide_exponents_and_other_key_sizes(self):
        source = os.environ.get("WOLFSSL_RSA_SOURCE")
        candidates = list(ROOT.glob(".pio/libdeps/*/Arduino-wolfSSL/src/wolfcrypt/src/rsa.c"))
        if not source and not candidates:
            self.skipTest("Install Arduino-wolfSSL or set WOLFSSL_RSA_SOURCE")
        path = self.root / "rsa.c"
        path.write_text(Path(source or candidates[0]).read_text())
        self.module["patch_rsa_fallback"](path)
        text = path.read_text()
        start = text.rindex("static int wc_RsaFunctionSync(", 0, text.index(self.module["RSA_MARKER"]))
        function = text[start:text.index("\n}\n", start) + 3]
        cfile = self.root / "check.c"
        cfile.write_text(HARNESS + function + CHECKS)
        for enabled in (False, True):
            with self.subTest(enabled=enabled):
                flags = ["-DFREEINK_WOLFSSL_SP_RSA_FALLBACK"] if enabled else []
                binary = self.root / "check"
                subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                                "-Wno-unused-function", "-fsanitize=address,undefined",
                                "-DWOLFSSL_HAVE_SP_RSA", *flags, str(cfile), "-o", str(binary)], check=True)
                subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
