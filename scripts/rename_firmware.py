"""
PlatformIO post-build script: copy firmware.bin to firmware-<env>.bin
in the same build directory.

Example outputs:
  .pio/build/tiny/firmware-tiny.bin
  .pio/build/xlarge/firmware-xlarge.bin
  .pio/build/no_emoji/firmware-no_emoji.bin
"""

import shutil
import sys


def rename_firmware(source, target, env):
    import os
    env_name = env['PIOENV']
    src = str(target[0])
    dst = os.path.join(os.path.dirname(src), f'firmware-{env_name}.bin')
    shutil.copy(src, dst)
    print(f'Firmware copied to: {dst}')


try:
    Import('env')                                           # noqa: F821  # type: ignore[name-defined]
    env.AddPostAction(                                      # noqa: F821  # type: ignore[name-defined]
        '$BUILD_DIR/${PROGNAME}.bin',
        rename_firmware,
    )
except NameError:
    print('rename_firmware.py: must be run via PlatformIO', file=sys.stderr)
