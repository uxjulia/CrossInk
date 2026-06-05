# Reading-Stats Backup and Format-Safety - Implementation Plan

- **Branch:** `feat/stats-backup`
- **Status:** Ready for implementation (design approved)
- **Audience:** Implementer picking this up cold. All decisions are resolved; see "Resolved decisions" at the end.

> Line numbers below are anchors as of this writing. Verify against the live file before editing, since the surrounding code moves.

---

## 1. Background and root cause

Global reading stats are stored as a single binary file `/.crosspoint/global_stats.bin` with a versioned layout. The current format is **v3 (159 bytes)**, defined in `src/activities/reader/GlobalReadingStats.h:20-21` (`CURRENT_FILE_VERSION = 3`, `CURRENT_FILE_SIZE = 159`).

A user lost all stats by flashing an **older v2 build** onto a device whose file was already **v3**:

1. The v2 loader did not recognize a 159-byte / version-3 file and treated it as "corrupt," resetting in-memory stats to zero.
2. On the next save, the rolling-backup rotation (`remove(.bak); rename(primary -> .bak)`) pushed the real v3 data into `.bak`.
3. A second save rotated the now-empty primary over `.bak`, destroying the only good copy.

Two independent problems to fix:

- **Format safety:** a build must never overwrite a stats file written by a *newer* format version.
- **Durability:** stats live only inside `/.crosspoint/`, which users routinely delete while troubleshooting. They need backups that survive that.

## 2. Goals and non-goals

**Goals**

- A build never overwrites or rotates away a stats file whose on-disk version is newer than the build understands.
- Reading stats survive deletion of `/.crosspoint/`.
- X3 (has DS3231 RTC) gets real-dated backups, automatic on sleep. X4 (no RTC) gets manual backups with non-dated names.

**Non-goals (this iteration)**

- In-device "restore from backup" UI. Manual SD-card restore is acceptable for now and should be documented.
- Backing up per-book stats or synced peer files. Global cumulative stats only.

---

## 3. Part 1 - Non-destructive format guard

All changes in `src/activities/reader/GlobalReadingStats.cpp`.

### Current mechanics (the data-loss path)

- `loadFromOpenFile()` (`:126-155`) returns `false` for any unrecognized size/version, so a truly corrupt file and a *newer-version* file are indistinguishable.
- `load()` (`:215-224`) then silently returns fresh (zeroed) stats.
- `save()` (`:279-284`) -> `saveToFile()` (`:166-204`) unconditionally does `remove(.bak); rename(primary -> .bak)` before writing, so an empty save clobbers the only good copy.

### Changes

1. **Distinguish "newer version" from "corrupt."** In `loadFromOpenFile()`, read the version byte first and compare `data[0]` against `GlobalReadingStats::CURRENT_FILE_VERSION`. If `data[0] > CURRENT_FILE_VERSION` (or the file is larger than `CURRENT_FILE_SIZE` with an unknown version), report a distinct **NewerFormat** result rather than `false`. Suggested: change the return type to `enum class StatsLoadResult { Ok, Invalid, NewerFormat }`.

2. **Latch a session-wide save block.** Add a file-local `static bool s_blockDestructiveSave = false;` in the anonymous namespace. In `load()`, if the primary file reads as `NewerFormat`, set the latch, log `LOG_ERR("GSTATS", "On-disk stats are from a newer build (vN); refusing to overwrite")`, and return fresh stats (display shows 0 this session, but nothing is destroyed). A process-wide latch is correct here because stats are a single global concept and the in-memory struct is copied around (for example `loadAggregated`), so a per-instance flag is easy to lose.

3. **Honor the latch in `save()`.** First line: `if (s_blockDestructiveSave) { LOG_ERR(...); return; }`. This is the actual data-loss stopper.

4. **Make rotation atomic (hardening, independent of the latch).** Rework `saveToFile()` to: serialize to `global_stats.bin.tmp`, `flush`/`sync`/`close`, verify the byte count, then rotate the current valid primary into `.bak`, then `rename(tmp -> primary)`. Invariant: `.bak` is only ever a copy of a previously-good primary, and a failed or short write never destroys the existing primary. 159 bytes, so the temp file is free.

### Scope note

This protects **forward only**: a future v4 file read by this v3 build. It cannot retroactively teach already-flashed older builds to read v3. The durable cross-version protection is Part 2.

### Tests (simulator + device)

- Hand-craft a 159-byte file with version byte `0xFF`. Boot, confirm the "refusing to overwrite" log, and confirm the file is byte-identical after a reading session plus a sleep (must not shrink to 17 bytes or get rotated).
- Normal v3 round-trip still works; v1 (13-byte) and v2 (17-byte) files still upgrade-load.
- Simulate a short write and confirm `.bak` still holds good data.

---

## 4. Part 2 - Dated stats backups outside `/.crosspoint/`

### 4.1 Behavior spec

Backups are written to `/.crossink-stats-backup/` at the SD-card root (sibling of `/.crosspoint/`, so it survives deletion of that folder).

| | X3 (RTC present) | X4 (no RTC) |
|---|---|---|
| Manual "Back up now" | `stats_YYYY-MM-DD_HHMM.bin` | `stats_backup_NNN.bin` (incrementing) |
| Automatic | On every sleep, overwrite `stats_YYYY-MM-DD.bin` (one rolling file per day) | none |

- Date source is `getCurrentLocalReadingStatsDateTime()` (`src/activities/reader/ReadingStatsUtils.h:42`). It returns `false` when there is no RTC or the clock is unset, so its return value is the gate: a successful read means use the dated naming, a `false` means fall back to the X4 incrementing scheme. This also makes an X3 with an unset clock degrade gracefully instead of writing a garbage date.
- `ReadingStatsDate` fields are `uint16_t year; uint8_t month; uint8_t day;` (`ReadingStatsUtils.h:14-21`); `ReadingStatsDateTime` adds `hour/minute/second`.

### 4.2 Automatic backup on sleep (the "invisible" mechanism)

Requirement: the backup should happen on sleep without the user noticing a delay - the device should already look asleep when the file is written.

This works because of where deep sleep commits. In `src/main.cpp` `enterDeepSleep()` (`:608-632`):

```
deepSleepInProgress = true;             // :622
activityManager.goToSleep(fromTimeout); // :623  runs the outgoing reader onExit (saves stats),
                                        //       renders the sleep cover, powers off the e-ink panel
                                        // <-- INSERT AUTO-BACKUP HERE
display.deepSleep();                    // :632
... esp_deep_sleep_start()              // HalPowerManager.cpp:116 / HalGPIO.cpp:242
```

At the insertion point:

- `goToSleep()` is synchronous and blocking. When it returns, the sleep cover is already on the panel and the panel is powered off (`TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH`), so the device looks asleep.
- SD and CPU are still powered; deep sleep has not started.
- The outgoing reader activity's `onExit()` runs inside `goToSleep()` (see the comment at `main.cpp:620`), and that is what persists the session's stats. So `global_stats.bin` on disk is already the freshest copy. The backup just copies the file.

A 159-byte copy is single-digit to low-tens of milliseconds, invisible behind the already-displayed sleep screen.

**Rules for the hook:**

- Only run when `gpio.deviceIsX3()` and `SETTINGS.autoBackupStats` is on and a date is available.
- Overwrite today's `stats_YYYY-MM-DD.bin` each sleep, so the daily file is always the last-sleep snapshot.
- After writing, prune to 7 most recent files.
- **Best-effort:** wrap in error handling. A slow or failed backup must log and continue, never delaying `esp_deep_sleep_start()`. Do not introduce unbounded work on this path.

### 4.3 Manual backup and UI

Location: **Settings > System > Files & Cache**. That submenu is built in `src/SettingsList.h` `buildSystemFilesCacheSettingsList()` (`:732`), where the Clear Cache action is already wired (`:737`). Add next to it:

- `SettingInfo::Action(StrId::STR_BACKUP_READING_STATS, SettingAction::BackupStats)` (both X3 and X4).
- `SettingInfo::Toggle(StrId::STR_AUTO_BACKUP_STATS, &CrossPointSettings::autoBackupStats, "autoBackupStats", StrId::STR_CAT_SYSTEM)` - **X3 only**. Gate it with the same mechanism that hides clock/RTC entries on X4 (see the clock entries note at `SettingsList.h:459`).

Add a new `SettingAction::BackupStats` and handle it in `src/activities/settings/SettingsActivity.cpp`, launching a confirm -> act -> result-popup flow modeled on `ClearCacheActivity` (`src/activities/settings/ClearCacheActivity.cpp:76-121`). Show the resulting filename in the success popup so the user knows what was written.

### 4.4 New setting

- Add `bool autoBackupStats = true;` to `CrossPointSettings` (default on, for data safety) with persisted key `"autoBackupStats"`.

### 4.5 Retention and pruning

- Keep the 7 most recent files in `/.crossink-stats-backup/` (by name; dated and incrementing names both sort lexicographically), delete the rest.
- Use the directory-walk pattern already in `GlobalReadingStats::loadAggregated()` (`GlobalReadingStats.cpp:251-270`): `Storage.open(dir)` then `openNextFile()` with a stack `char name[128]` buffer. No heap churn.

### 4.6 i18n

Add strings to `lib/I18n/translations/*.yaml` and regenerate with `scripts/gen_i18n.py`. Do not hand-edit generated files under `lib/I18n/`. All user-facing text via `tr(STR_*)`. Suggested ids:

- `STR_BACKUP_READING_STATS`, `STR_AUTO_BACKUP_STATS`
- `STR_BACKUP_STATS_CONFIRM`, `STR_BACKUP_STATS_DONE` (include filename), `STR_BACKUP_STATS_FAILED`

---

## 5. New module: `StatsBackup`

Suggested files: `src/activities/reader/StatsBackup.{h,cpp}`. Stateless free functions.

- `bool backupGlobalStats(bool manual);`
  - Ensure `/.crossink-stats-backup/` exists (`Storage.ensureDirectoryExists` / `Storage.mkdir`, `HalStorage.h:31,34`).
  - Compute the filename per the table in 4.1 (manual flag selects timestamped vs daily; date availability selects dated vs incrementing).
  - Copy the on-disk `/.crosspoint/global_stats.bin` bytes (read up to 159 bytes, write). Copy the on-disk file, not the in-memory struct, so the backup reflects exactly what is persisted. Do not hold the source open while writing the destination (SD single-open rule): read fully, close, then write.
  - Return success/fail for the UI and the sleep hook.
- `int pruneBackups(int keep);` - list, sort by name, delete oldest beyond `keep`. Default `keep = 7`.
- Optional helper on `GlobalReadingStats`: `bool writeTo(const char* path) const;` reusing `saveToFile(path, /*backupPath=*/nullptr)` if you prefer serializing from memory; the file-copy approach above avoids needing it.

---

## 6. Files to add or change

**New**

- `src/activities/reader/StatsBackup.{h,cpp}`
- `src/activities/settings/BackupStatsActivity.{h,cpp}` (or fold the confirm/result flow into an existing settings activity)

**Edit**

- `src/activities/reader/GlobalReadingStats.{h,cpp}` - Part 1 guard + atomic rotation
- `src/main.cpp` - auto-backup hook in `enterDeepSleep()`
- `src/SettingsList.h` - menu entries (action + X3-only toggle)
- `src/CrossPointSettings.h` - new `autoBackupStats` field
- `src/activities/settings/SettingsActivity.cpp` - handle `SettingAction::BackupStats`
- `lib/I18n/translations/*.yaml` (+ regenerate)
- `CHANGELOG.md`

---

## 7. Verification (hardware)

- **X3, clock set:** sleep, wake, sleep again same day -> exactly one `stats_YYYY-MM-DD.bin`, updated to the latest stats. A manual backup adds a `_HHMM` file. Delete `/.crosspoint/` -> backups still present. Eighth distinct day -> oldest pruned, 7 remain.
- **X3, auto toggle off:** no file written on sleep; manual still works.
- **X3, clock unset:** falls back to incrementing names, no garbage dates.
- **X4:** manual backup writes `stats_backup_001.bin`, second `_002`; no auto-backup on sleep.
- **Part 1:** a newer-version file is never mutated across a session + sleep; pruning keeps the 7 most recent.
- **Sleep latency:** confirm no perceptible delay between the sleep screen appearing and the device sleeping (the write is behind the displayed screen).

## 8. Constraints and risks

- ESP32-C3, no PSRAM, ~380 KB RAM. 159-byte buffers and a once-per-sleep directory scan are negligible. Keep dir-walk buffers on the stack (`char name[128]`), no heap churn.
- SD single-open rule: never hold `global_stats.bin` open while writing the backup.
- Do not add latency or unbounded work to the deep-sleep path. The auto-backup must be best-effort and bounded.
- `new` is not nothrow on this platform; if any allocation is added, use `new (std::nothrow)` / `makeUniqueNoThrow<T>()`. The 159-byte buffers should be stack, so this likely does not apply.

## 9. Suggested commit breakdown

1. `fix: don't overwrite reading stats written by a newer format version` (Part 1: guard + atomic rotation).
2. `feat: add StatsBackup module writing dated backups to /.crossink-stats-backup` (Part 2 core + X3 on-sleep auto + pruning).
3. `feat: add Backup Reading Stats settings action and X3 auto-backup toggle` (UI + setting + i18n).

Add a `CHANGELOG.md` entry per the repo guide (Added/Fixed sections) describing the user-facing behavior.

---

## Resolved decisions

1. Manual backup lives in Settings > System > Files & Cache (`Backup Reading Stats`), for both X3 and X4. X3 additionally gets an `Automatic Backups: ON | OFF` toggle in the same place.
2. Auto-backup (X3) happens on sleep, written invisibly in `enterDeepSleep()` after the sleep screen is shown and the panel is powered off, before `esp_deep_sleep_start()`.
3. Retention: keep the last 7 backups.
4. In-device restore: out of scope for now (manual SD restore documented).
5. X4 manual naming: incrementing `stats_backup_NNN.bin`.
