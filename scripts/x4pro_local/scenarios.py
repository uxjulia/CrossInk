"""Five real-firmware acceptance scenarios, for the disposable sd-assets card.

Coordinates refer to the pinned X4 Pro's 480x800 portrait controller, Lyra theme,
English fallback labels and Small UI scale. Navigation failures fail explicitly.
"""
import re
import struct

from core import REFRESH, sha, write_json

BOOK = "/TestBook.epub"


def cache_path():
    value = 14695981039346656037
    for byte in BOOK.encode():
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return f"/.crosspoint/epub_{value}/progress.bin"


def progress(n):
    data = n.sd_file(cache_path())
    if len(data) < 6:
        raise AssertionError("Truncated EPUB progress")
    spine, page, pages = struct.unpack_from("<HHH", data)
    return dict(spine=spine, page=page, pages=pages)


def activity(n, x, y, name, timeout=None):
    after = n.mark()
    n.tap(x, y)
    n.wait_log(r"Entering activity: " + re.escape(name) + r"[\s\S]*" + REFRESH, after, timeout)


def home(n):
    after = n.mark()
    n.tap()
    n.wait_log(r"Entering activity: Home[\s\S]*" + REFRESH, after)


def acceptance(n, manifest):
    def passed(name, evidence):
        n.health()
        manifest["scenarios"].append(dict(name=name, status="passed", evidence=evidence))
        write_json(n.run / "manifest.json", manifest)
        print("PASS " + name, flush=True)

    passed("home-readiness", dict(board="xteink_x4_pro", screenshot="home.png",
                                  predicate="Home entry followed by UC8179 refresh completion"))
    activity(n, 150, 340, "FileBrowser")
    after = n.mark()
    n.tap(160, 140)
    n.wait_log(r"Entering activity: EpubReader", after)
    n.wait_log(r"Cache build complete: pages=\d+[\s\S]*8179_gray_split_DRF[\s\S]*Restored and freed BW buffer chunks", after, 90)
    first = n.shot("epub-page-one")
    after = n.mark()
    n.press("down")
    n.wait_log(r"8179_gray_split_DRF[\s\S]*Restored and freed BW buffer chunks", after)
    second = n.shot("epub-page-two")
    if sha(first) == sha(second):
        raise AssertionError("Page turn did not change the rendered panel")
    home(n)  # Reader exit flushes the debounced progress before assertion.
    position = progress(n)
    if position["spine"] != 0 or position["page"] != 1 or position["pages"] < 2:
        raise AssertionError(f"Page turn did not persist the second page: {position}")
    passed("epub-open-page-turn", dict(book=BOOK, progress=position,
                                      screenshots=[first.name, second.name]))

    # A recent book adds Reading Stats above File Transfer in Lyra Home.
    activity(n, 150, 590, "Settings")
    n.tap_refresh(420, 467)  # Display > Dark Mode (off -> on).
    activity(n, 25, 55, "Home")
    settings = n.settings()
    if settings.get("screenInverted") != 1:
        raise AssertionError("Dark Mode did not save screenInverted=1")
    passed("settings-change", dict(key="screenInverted", value=1))

    activity(n, 150, 590, "Settings")
    n.tap_refresh(420, 108)  # System tab.
    n.tap_refresh(180, 175)  # Device.
    n.tap_refresh(180, 390)  # Language.
    n.shot("language-picker")
    after = n.mark()
    n.tap(240, 485)  # Local Test, the only SD language.
    n.wait_log(r"Mapped ZZ-TEST .*generation=1", after, 90)
    n.ready(after, 90)
    settings = n.settings()
    if settings.get("language") != "ZZ-TEST" or settings.get("languageCacheGeneration") != 1:
        raise AssertionError("SD language selection/cache generation did not persist")
    n.shot("language-installed")

    activity(n, 150, 590, "Settings")
    n.tap_refresh(180, 108)  # Reader tab.
    n.tap_refresh(180, 235)  # Page Layout.
    activity(n, 180, 450, "HyphenationManager")
    n.shot("hyphenation-picker")
    n.tap_refresh(240, 258)  # French, the only source pack on this SD card.
    after = n.mark()
    n.tap(240, 458)  # Install / Update.
    n.wait_log(r"\[HYPH\] Mapped 1 packs \([1-9]\d* bytes\)", after, 90)
    n.ready(after, 90)
    n.shot("hyphenation-installed")
    passed("language-hyphenation-install-from-sd", dict(language="ZZ-TEST", generation=1,
        hyphenation_packs=1, source_pack="fr", screenshots=["language-installed.png", "hyphenation-installed.png"]))

    after = n.mark()
    n.qmp.call("system_reset")
    n.wait_log(r"Mapped ZZ-TEST .*generation=1", after, 90)
    n.wait_log(r"\[HYPH\] Mapped 1 packs", after, 90)
    n.ready(after, 90)
    settings = n.settings()
    if settings.get("screenInverted") != 1 or settings.get("language") != "ZZ-TEST":
        raise AssertionError("Settings or language lost across machine restart")
    if progress(n) != position:
        raise AssertionError("Saved EPUB position changed across restart")
    # Open the recent book after reset; its section must load from the SD cache.
    after = n.mark()
    n.tap(250, 175)
    n.wait_log(r"Entering activity: EpubReader", after)
    # Dark Mode renders white foreground text without the text grayscale pass.
    n.wait_log(r"Cache found, skipping build[\s\S]*" + REFRESH, after, 90)
    n.shot("reopened-after-restart")
    home(n)
    if progress(n) != position:
        raise AssertionError("Reopening the EPUB lost its saved position")
    passed("restart-persistence", dict(progress=position, screenInverted=1,
        language="ZZ-TEST", hyphenation_packs=1, screenshot="reopened-after-restart.png"))
