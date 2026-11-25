"""
PlatformIO pre-build script: apply CrossPoint's JPEGDEC patches via `git apply`.

The upstream JPEGDEC pin still has progressive grayscale bugs in
JPEGDecodeMCU_P, including unsafe MCU_SKIP writes and attempts to consume
chroma components that are absent from the current scan. Non-interleaved
luma scans also need their own block grid to avoid shifted image rows.
The patches in `scripts/jpegdec_patches/` carry the fix; this script applies
each one against the libdep working tree.

Each patch's idempotency is decided by git itself:
  * `git apply --check --reverse` succeeds  -> already applied, skip
  * source already contains the equivalent fix -> already applied, skip
  * `git apply --check`            succeeds  -> apply
  * neither succeeds                          -> abort the build

Patches live in `scripts/jpegdec_patches/` as one-commit-per-fix files
(see the file headers for context). Applied in lexical order.
"""

try:
    Import("env")  # noqa: F821 (SCons-injected global)
except NameError:
    env = None
import os
import subprocess
import sys


PROJECT_DIR = env["PROJECT_DIR"] if env is not None else os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATCH_DIR = os.path.join(PROJECT_DIR, "scripts", "jpegdec_patches")


def patch_jpegdec(env):
    libdeps_dir = os.path.join(env["PROJECT_DIR"], ".pio", "libdeps")
    if not os.path.isdir(libdeps_dir):
        return
    patches = _patch_files()
    for env_dir in os.listdir(libdeps_dir):
        jpeg_dir = os.path.join(libdeps_dir, env_dir, "JPEGDEC")
        if not os.path.isdir(os.path.join(jpeg_dir, ".git")):
            continue
        for patch in patches:
            _apply_one(jpeg_dir, patch)


def _patch_files():
    if not os.path.isdir(PATCH_DIR):
        raise RuntimeError(
            "JPEGDEC patches missing -- aborting build (expected directory %s)"
            % PATCH_DIR
        )
    patches = sorted(
        os.path.join(PATCH_DIR, name)
        for name in os.listdir(PATCH_DIR)
        if name.endswith(".patch")
    )
    if not patches:
        raise RuntimeError(
            "JPEGDEC patches missing -- aborting build (no .patch files in %s)"
            % PATCH_DIR
        )
    return patches


def _apply_one(jpeg_dir, patch_path):
    name = os.path.basename(patch_path)
    if _git_apply_succeeds(jpeg_dir, patch_path, reverse=True):
        return
    if _patch_already_satisfied(jpeg_dir, name):
        return
    if not _git_apply_succeeds(jpeg_dir, patch_path, reverse=False):
        # Not applied, not appliable -- the libdep source has diverged from
        # what the patch expects. Don't write a half-patched file.
        result = subprocess.run(
            ["git", "apply", "--check", patch_path],
            cwd=jpeg_dir,
            capture_output=True,
            text=True,
        )
        sys.stderr.write(
            "ERROR: JPEGDEC patch %s does not apply cleanly:\n%s%s\n"
            % (name, result.stdout, result.stderr)
        )
        raise SystemExit(1)
    subprocess.run(["git", "apply", patch_path], cwd=jpeg_dir, check=True)
    print("Applied JPEGDEC patch: %s" % name)


def _patch_already_satisfied(jpeg_dir, patch_name):
    jpeg_inl = os.path.join(jpeg_dir, "src", "jpeg.inl")
    if not os.path.isfile(jpeg_inl):
        return False
    with open(jpeg_inl, "r", encoding="utf-8") as f:
        content = f.read()

    if patch_name.startswith("0001-"):
        return (
            "signed short *pMCU = (iMCU < 0) ? pJPEG->sMCUs" in content
            and ": &pJPEG->sMCUs[iMCU & 0xffffff];" in content
        )

    if patch_name.startswith("0002-"):
        return (
            "if (iMCU >= 0)\n                    pMCU[0] |= iPositive;" in content
            and "if (iMCU >= 0)\n            pMCU[0] = (short)*iDCPredictor;" in content
        )

    if patch_name.startswith("0003-"):
        return (
            "if (pJPEG->JPCI[1].component_needed)" in content
            and "if (pJPEG->JPCI[2].component_needed) {" in content
        )

    return False


def _git_apply_succeeds(jpeg_dir, patch_path, *, reverse):
    cmd = ["git", "apply", "--check"]
    if reverse:
        cmd.append("--reverse")
    cmd.append(patch_path)
    return subprocess.run(
        cmd, cwd=jpeg_dir, capture_output=True, text=True
    ).returncode == 0


if env is not None:
    patch_jpegdec(env)
elif __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("jpeg_dir", help="Pinned JPEGDEC checkout to patch for host tests")
    args = parser.parse_args()
    for patch in _patch_files():
        _apply_one(args.jpeg_dir, patch)
