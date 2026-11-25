#!/usr/bin/env python3
"""Verify reader control migrations and complete shortcut lists through the real web API."""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
SHORTCUT_KEYS = (
    'shortPwrBtn', 'longPwrBtn', 'powerChordAction', 'sideButtonChordAction',
    'sideButtonUpShort', 'sideButtonUpLong', 'sideButtonDownShort', 'sideButtonDownLong',
    'frontButtonLeftLong', 'frontButtonRightLong', 'longPressMenuAction', 'longPressBackAction',
    'homeButtonTapAction', 'homeButtonDoubleTapAction', 'homeButtonLongPressAction',
    'twoFingerSwipeUp', 'twoFingerSwipeDown', 'twoFingerSwipeLeft', 'twoFingerSwipeRight',
    'leftEdgeUp', 'leftEdgeDown', 'rightEdgeUp', 'rightEdgeDown',
)


def run(env_name):
    with tempfile.TemporaryDirectory(prefix='reader-control-api-') as directory:
        work = Path(directory)
        state = work / 'fs_' / '.crosspoint'
        state.mkdir(parents=True)
        settings_file = state / 'crossink-settings.json'
        settings_file.write_text(json.dumps({'touchReaderControls': 0, 'longPressButtonBehavior': 1}))
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            port = probe.getsockname()[1]
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(('CROSSPOINT_SIM_', 'CROSSINK_SIMULATOR_SMOKE'))}
        env.update(SDL_VIDEODRIVER='dummy', CROSSPOINT_SIM_HTTP_PORT=str(port),
                   CROSSPOINT_SIM_SILENT_REBOOT_MAGIC=str(0xC1EAB007),
                   CROSSPOINT_SIM_SILENT_REBOOT_TARGET='6', CROSSPOINT_SIM_SILENT_REBOOT_PAYLOAD='2')
        with (work / 'simulator.log').open('w') as log:
            process = subprocess.Popen([str(ROOT / '.pio/build' / env_name / 'program')],
                                       cwd=work, env=env, stdout=log, stderr=subprocess.STDOUT)
            try:
                url = f'http://127.0.0.1:{port}/api/settings'

                def read():
                    with urllib.request.urlopen(url, timeout=5) as response:
                        return {s['key']: s for s in json.load(response)}

                deadline = time.monotonic() + 30
                while True:
                    try:
                        settings = read()
                        break
                    except OSError:
                        if process.poll() is not None or time.monotonic() >= deadline:
                            raise RuntimeError('Simulator web API did not start')
                        time.sleep(0.1)
                assert 'touchReaderControls' not in settings
                assert 'longPressButtonBehavior' not in settings
                assert len(settings['frontButtonLeftLong']['options']) > 20
                for key, label in [('frontButtonLeftLong', 'Previous Chapter'),
                                   ('frontButtonRightLong', 'Next Chapter')]:
                    assert settings[key]['options'][settings[key]['value']] == label, key
                for track_stats in (1, 0):
                    request = urllib.request.Request(url, data=json.dumps({'trackReadingStats': track_stats}).encode(),
                                                     headers={'Content-Type': 'application/json'})
                    with urllib.request.urlopen(request, timeout=5) as response:
                        assert response.status == 200
                    settings = read()
                    for key in SHORTCUT_KEYS:
                        value = settings[key]['options'].index('Disable Page Turn Gestures')
                        request = urllib.request.Request(url, data=json.dumps({key: value}).encode(),
                                                         headers={'Content-Type': 'application/json'})
                        with urllib.request.urlopen(request, timeout=5) as response:
                            assert response.status == 200
                        # Two-finger actions are exclusive; check each immediately after assigning it.
                        assert read()[key]['value'] == value, (key, track_stats)
                saved = json.loads(settings_file.read_text())
                assert 'touchReaderControls' not in saved and 'longPressButtonBehavior' not in saved
                assert saved['disablePageTurnGestures'] == 0
                print('PASS: retired settings absent; all 23 shortcut settings stream and save the new action')
            except BaseException:
                log.flush()
                print((work / 'simulator.log').read_text()[-4000:])
                raise
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--env', default='x4-pro-simulator')
    run(parser.parse_args().env)
