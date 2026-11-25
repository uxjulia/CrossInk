from pathlib import Path

Import("env")


PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
MARKER = "/* CrossPoint wolfSSL compatibility overrides */"
OVERRIDES = f"""

{MARKER}
#undef NO_DH
#ifndef HAVE_FFDHE_2048
#define HAVE_FFDHE_2048
#endif
#undef FP_MAX_BITS
#define FP_MAX_BITS 8192
/* Arduino-wolfSSL turns on DEBUG_WOLFSSL, which compiles every WOLFSSL_MSG and
   WOLFSSL_ENTER trace string into flash. Keep it only for FREEINK_WOLFSSL_DEBUG. */
#ifndef FREEINK_WOLFSSL_DEBUG
#undef DEBUG_WOLFSSL
#endif
"""


def patch_user_settings(path: Path) -> None:
    original_text = path.read_text()
    text = original_text
    if MARKER in text:
        text = text.split(MARKER, 1)[0].rstrip()
    patched_text = text.rstrip() + OVERRIDES + "\n"
    if original_text == patched_text:
        return
    path.write_text(patched_text)
    print(f"Patched wolfSSL settings: {path.relative_to(PROJECT_DIR)}")


# wolfSSL grows its input buffer to fit each record larger than the static
# buffer, then frees it again as soon as the record is consumed. A server that
# ignores max_fragment_length sends 16 KB records, so a large download on a
# PSRAM-less C3 needs a fresh ~17 KB contiguous block for every record, and
# fails with MEMORY_E once fragmentation leaves none. Keep the grown buffer
# until the connection is freed, reusing it for records that fit. A later,
# larger record can still require growth; release a consumed buffer first so
# growth does not require both the old and new allocations simultaneously.
SHRINK_MARKER = "/* CrossInk: keep a grown input buffer until the connection is freed */"
SHRINK_ORIGINAL = """    if (!forcedFree && (usedLength > STATIC_BUFFER_LEN ||
            ssl->buffers.clearOutputBuffer.length > 0))
        return;
"""
# The source lives in PlatformIO's dependency cache. Keep the original branch
# available so reverting this script also disables retention in an already
# patched dependency, without requiring users to delete their package cache.
SHRINK_PATCHED = f"""    {SHRINK_MARKER}
#if defined(FREEINK_WOLFSSL_RETAIN_INPUT_BUFFER)
    if (!forcedFree)
        return;
#else
{SHRINK_ORIGINAL}#endif
"""
env.Append(CPPDEFINES=["FREEINK_WOLFSSL_RETAIN_INPUT_BUFFER", "FREEINK_WOLFSSL_RELEASE_EMPTY_INPUT_BUFFER"])


def patch_input_buffer_shrink(path: Path) -> None:
    text = path.read_text()
    if SHRINK_MARKER in text:
        if text.count(SHRINK_PATCHED) != 1:
            raise RuntimeError(f"Corrupt wolfSSL input buffer patch in {path}")
        return
    if text.count(SHRINK_ORIGINAL) != 1:
        raise RuntimeError(f"Unsupported wolfSSL ShrinkInputBuffer in {path}; refusing an unverified patch")
    path.write_text(text.replace(SHRINK_ORIGINAL, SHRINK_PATCHED, 1))
    print(f"Patched wolfSSL input buffer: {path.relative_to(PROJECT_DIR)}")


# A separate flag lets an older build disable this change even when its
# dependency cache still contains the patched source.
GROW_MARKER = "/* CrossInk: release a consumed input buffer before growing it */"
GROW_ORIGINAL = """    tmp = (byte*)XMALLOC(size + usedLength + align,
                             ssl->heap, DYNAMIC_TYPE_IN_BUFFER);
    WOLFSSL_MSG("growing input buffer");
"""
GROW_PATCHED = f"""    {GROW_MARKER}
#if defined(FREEINK_WOLFSSL_RELEASE_EMPTY_INPUT_BUFFER)
    /* No unread ciphertext or pending plaintext may refer to this buffer.
     * ShrinkInputBuffer zeroes/frees it and restores valid static-buffer state,
     * including on allocation failure below. Records that fit never get here. */
    if (ssl->buffers.inputBuffer.dynamicFlag && usedLength == 0 &&
            ssl->buffers.inputBuffer.length == ssl->buffers.inputBuffer.idx &&
            ssl->buffers.clearOutputBuffer.length == 0) {{
        ShrinkInputBuffer(ssl, FORCED_FREE);
    }}
#endif
{GROW_ORIGINAL}"""


def patch_input_buffer_growth(path: Path) -> None:
    text = path.read_text()
    if GROW_MARKER in text:
        if text.count(GROW_PATCHED) != 1:
            raise RuntimeError(f"Corrupt wolfSSL input growth patch in {path}")
        return
    if text.count(GROW_ORIGINAL) != 1:
        raise RuntimeError(f"Unsupported wolfSSL GrowInputBuffer in {path}; refusing an unverified patch")
    path.write_text(text.replace(GROW_ORIGINAL, GROW_PATCHED, 1))
    print(f"Patched wolfSSL input growth: {path.relative_to(PROJECT_DIR)}")


for settings in PROJECT_DIR.glob(".pio/libdeps/*/Arduino-wolfSSL/src/user_settings.h"):
    patch_user_settings(settings)

for internal in PROJECT_DIR.glob(".pio/libdeps/*/Arduino-wolfSSL/src/src/internal.c"):
    patch_input_buffer_shrink(internal)
    patch_input_buffer_growth(internal)


# SP RSA public operations accept exponents up to 64 bits. Return the existing
# fallback signal for wider exponents instead of letting SP reject a key that
# general math previously supported. Keep the patch inert in older builds that
# reuse this dependency cache without the accompanying define.
RSA_MARKER = "/* CrossInk: preserve general RSA for wide public exponents */"
RSA_ORIGINAL = """    ret = RsaFunction_SP(in, inLen, out, outLen, type, key, rng);
"""
RSA_PATCHED = f"""    {RSA_MARKER}
#if defined(FREEINK_WOLFSSL_SP_RSA_FALLBACK) && !defined(WOLFSSL_SP_MATH)
    if ((type == RSA_PUBLIC_ENCRYPT || type == RSA_PUBLIC_DECRYPT) &&
            mp_count_bits(&key->e) > 64)
        ret = WC_KEY_SIZE_E;
    else
#endif
{RSA_ORIGINAL}"""
env.Append(CPPDEFINES=["FREEINK_WOLFSSL_SP_RSA_FALLBACK"])


def patch_rsa_fallback(path: Path) -> None:
    text = path.read_text()
    if RSA_MARKER in text:
        if text.count(RSA_PATCHED) != 1:
            raise RuntimeError(f"Corrupt wolfSSL RSA fallback patch in {path}")
        return
    if text.count(RSA_ORIGINAL) != 1:
        raise RuntimeError(f"Unsupported wolfSSL RSA dispatch in {path}; refusing an unverified patch")
    path.write_text(text.replace(RSA_ORIGINAL, RSA_PATCHED, 1))
    print(f"Patched wolfSSL RSA fallback: {path.relative_to(PROJECT_DIR)}")


for rsa in PROJECT_DIR.glob(".pio/libdeps/*/Arduino-wolfSSL/src/wolfcrypt/src/rsa.c"):
    patch_rsa_fallback(rsa)
