"""Keep vendor recovery intact: upload only the selected application slot."""
import csv
from pathlib import Path

Import("env")

partition_file = Path(env['PROJECT_DIR']) / env.GetProjectOption('board_build.partitions')
app_label = env.GetProjectOption('custom_firmware_app_partition', 'app')
with partition_file.open(newline='', encoding='utf-8') as table:
    slots = [row for row in csv.reader(table)
             if len(row) >= 5 and row[0].strip() == app_label and row[1].strip() == 'app']
if len(slots) != 1 or not slots[0][3].strip():
    raise RuntimeError('App-only upload requires one selected app slot with an explicit offset')
app_offset = int(slots[0][3].strip(), 0)

# PlatformIO can already have copied these pairs into UPLOADERFLAGS before a
# post script runs. Clearing FLASH_EXTRA_IMAGES alone does not remove them.
flags = list(env.get('UPLOADERFLAGS', []))
for offset, image in env.get('FLASH_EXTRA_IMAGES', []):
    pair = [env.subst(str(offset)), env.subst(str(image))]
    index = 0
    while index + 1 < len(flags):
        if [env.subst(str(item)) for item in flags[index:index + 2]] == pair:
            del flags[index:index + 2]
        else:
            index += 1

env.Replace(FLASH_EXTRA_IMAGES=[], UPLOADERFLAGS=flags, ESP32_APP_OFFSET=hex(app_offset))
