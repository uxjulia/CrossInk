"""Compile the pinned wolfSSL buffer functions with a constrained host allocator.

Set WOLFSSL_INTERNAL_SOURCE to the installed Arduino-wolfSSL src/src/internal.c
when dependencies live outside this checkout. No network or device is needed.
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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned char byte;
typedef uint32_t word32;
#define STATIC_BUFFER_LEN 5
#define DTLS_RECORD_HEADER_SZ 13
#define MEMORY_E -125
#define BAD_FUNC_ARG -173
#define FORCED_FREE 1
#define DYNAMIC_TYPE_IN_BUFFER 0
#define WOLFSSL_MSG(x) ((void)0)
#define XMEMCPY memcpy
#define ForceZero(p, n) memset(p, 0, n)
#define IsEncryptionOn(s, r) 1
#define XMALLOC(n, h, t) alloc_buffer(n)
#define XFREE(p, h, t) free_buffer(p)
typedef union { size_t size; max_align_t alignment; } Header;
static size_t live, budget;
static void *alloc_buffer(size_t size) {
    if (size > budget - live) return NULL;
    Header *h = malloc(sizeof(Header) + size);
    assert(h);
    h->size = size;
    live += size;
    return h + 1;
}
static void free_buffer(void *ptr) {
    Header *h = (Header *)ptr - 1;
    assert(live >= h->size);
    live -= h->size;
    free(h);
}
typedef struct {
    byte *buffer;
    byte staticBuffer[STATIC_BUFFER_LEN];
    word32 bufferSize, length, idx;
    byte dynamicFlag, offset;
} Buffer;
typedef struct {
    struct { Buffer inputBuffer, clearOutputBuffer; } buffers;
    struct { int dtls; } options;
    void *heap;
} WOLFSSL;
'''
CHECKS = r'''
static WOLFSSL fresh(void) {
    WOLFSSL ssl = {0};
    ssl.buffers.inputBuffer.bufferSize = STATIC_BUFFER_LEN;
    return ssl;
}
static void init(WOLFSSL *ssl) {
    *ssl = fresh();
    ssl->buffers.inputBuffer.buffer = ssl->buffers.inputBuffer.staticBuffer;
    ssl->options.dtls = WOLFSSL_GENERAL_ALIGNMENT != 0;
    budget = 20000;
    assert(live == 0);
    assert(GrowInputBuffer(ssl, 8192, 0) == 0);
    memset(ssl->buffers.inputBuffer.buffer, 0x5a, 8192);
    ssl->buffers.inputBuffer.idx = ssl->buffers.inputBuffer.length = 8192;
}
int main(void) {
    WOLFSSL ssl;
    init(&ssl);
    byte *old = ssl.buffers.inputBuffer.buffer;
    ShrinkInputBuffer(&ssl, 0);
    assert(ssl.buffers.inputBuffer.buffer == old); /* retention still works */
    int result = GrowInputBuffer(&ssl, 16416, 0);
#ifdef EXPECT_OLD_FAILURE
    assert(result == MEMORY_E); /* reproduces old + new peak allocation */
    assert(ssl.buffers.inputBuffer.buffer == old);
#else
    assert(result == 0);
    assert(ssl.buffers.inputBuffer.bufferSize == 16416);
    assert(live <= 16416 + WOLFSSL_GENERAL_ALIGNMENT);
#endif
    ShrinkInputBuffer(&ssl, FORCED_FREE);
    assert(live == 0);

    /* Failed growth leaves a valid, empty buffer and supports cleanup/retry. */
    init(&ssl);
    budget = live;
    assert(GrowInputBuffer(&ssl, 16416, 0) == MEMORY_E);
#ifndef EXPECT_OLD_FAILURE
    assert(live == 0);
    assert(ssl.buffers.inputBuffer.dynamicFlag == 0);
    assert(ssl.buffers.inputBuffer.buffer == ssl.buffers.inputBuffer.staticBuffer);
    assert(ssl.buffers.inputBuffer.idx == 0 && ssl.buffers.inputBuffer.length == 0);
    budget = 20000;
    assert(GrowInputBuffer(&ssl, 16416, 0) == 0);
#endif
    if (ssl.buffers.inputBuffer.dynamicFlag) ShrinkInputBuffer(&ssl, FORCED_FREE);
    assert(live == 0);

    /* Unread ciphertext must survive both failed and successful growth. */
    init(&ssl);
    ssl.buffers.inputBuffer.idx = 17;
    ssl.buffers.inputBuffer.length = 20;
    old = ssl.buffers.inputBuffer.buffer;
    memcpy(old + 17, "abc", 3);
    assert(GrowInputBuffer(&ssl, 16416, 3) == MEMORY_E);
    assert(ssl.buffers.inputBuffer.buffer == old);
    assert(memcmp(old + 17, "abc", 3) == 0);
    budget = 40000;
    assert(GrowInputBuffer(&ssl, 16416, 3) == 0);
    assert(ssl.buffers.inputBuffer.idx == 0 && ssl.buffers.inputBuffer.length == 3);
    assert(memcmp(ssl.buffers.inputBuffer.buffer, "abc", 3) == 0);
    ShrinkInputBuffer(&ssl, FORCED_FREE);
    assert(live == 0);

    /* Pending plaintext can still refer into a consumed ciphertext buffer. */
    init(&ssl);
    old = ssl.buffers.inputBuffer.buffer;
    ssl.buffers.clearOutputBuffer.length = 1;
    assert(GrowInputBuffer(&ssl, 16416, 0) == MEMORY_E);
    assert(ssl.buffers.inputBuffer.buffer == old);
    ssl.buffers.clearOutputBuffer.length = 0;
    assert(GrowInputBuffer(&ssl, -1, 0) == BAD_FUNC_ARG);
    assert(ssl.buffers.inputBuffer.buffer == old);
    ShrinkInputBuffer(&ssl, FORCED_FREE);
    assert(live == 0);
    return 0;
}
'''


class GrowthPatchTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

        class Env:
            def subst(inner, value): return str(self.root)
            def Append(inner, **kwargs): pass

        self.module = runpy.run_path(str(ROOT / "scripts/patch_wolfssl.py"),
                                    init_globals={"Import": lambda _: None, "env": Env()})

    def test_exact_idempotent_and_rejects_unknown_or_corrupt_source(self):
        path = self.root / "internal.c"
        path.write_text(self.module["GROW_ORIGINAL"])
        patch = self.module["patch_input_buffer_growth"]
        patch(path)
        once = path.read_bytes()
        patch(path)
        self.assertEqual(once, path.read_bytes())
        for invalid in ("unknown source", self.module["GROW_MARKER"]):
            path.write_text(invalid)
            with self.assertRaises(RuntimeError): patch(path)
            self.assertEqual(path.read_text(), invalid)

    def test_actual_functions_with_limited_heap(self):
        source = os.environ.get("WOLFSSL_INTERNAL_SOURCE")
        candidates = list(ROOT.glob(".pio/libdeps/*/Arduino-wolfSSL/src/src/internal.c"))
        if not source and not candidates:
            self.skipTest("Install Arduino-wolfSSL or set WOLFSSL_INTERNAL_SOURCE")
        path = self.root / "internal.c"
        path.write_text(Path(source or candidates[0]).read_text())
        self.module["patch_input_buffer_shrink"](path)
        self.module["patch_input_buffer_growth"](path)
        text = path.read_text()
        functions = ""
        for signature in ("void ShrinkInputBuffer(", "int GrowInputBuffer("):
            start = text.index(signature)
            end = text.index("\n}\n", start) + 3
            functions += text[start:end]
        for old in (True, False):
            for alignment in (0, 16):
                with self.subTest(old=old, alignment=alignment):
                    cfile = self.root / "check.c"
                    binary = self.root / "check"
                    cfile.write_text(HARNESS + functions + CHECKS)
                    flags = ["-DEXPECT_OLD_FAILURE"] if old else ["-DFREEINK_WOLFSSL_RELEASE_EMPTY_INPUT_BUFFER"]
                    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                                    "-fsanitize=address,undefined", "-DFREEINK_WOLFSSL_RETAIN_INPUT_BUFFER",
                                    f"-DWOLFSSL_GENERAL_ALIGNMENT={alignment}", *flags,
                                    str(cfile), "-o", str(binary)], check=True)
                    subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
