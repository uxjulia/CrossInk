#!/usr/bin/env python3
"""Check Rotate Page 180 through the simulator's production settings API.

Run the simulator smoke with CROSSINK_SIMULATOR_SMOKE_ROTATE_SHORTCUTS=1 and
CROSSINK_SIMULATOR_SMOKE_ROTATE_WEB=1 first. Pass --home on a Home-key profile.
The check restores the original shortcut selections after testing save/reload.
"""
import argparse
import json
from urllib.request import Request, urlopen


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base-url', default='http://127.0.0.1:8080')
    parser.add_argument('--home', action='store_true')
    args = parser.parse_args()
    url = args.base_url.rstrip('/') + '/api/settings'

    def read():
        with urlopen(url, timeout=10) as response:
            return {setting['key']: setting for setting in json.load(response)}

    def save(values):
        request = Request(url, json.dumps(values).encode(), {'Content-Type': 'application/json'})
        with urlopen(request, timeout=10) as response:
            assert response.status == 200

    settings = read()
    keys = ['shortPwrBtn', 'longPwrBtn', 'powerChordAction', 'longPressMenuAction',
            'longPressBackAction', 'frontButtonLeftLong', 'frontButtonRightLong', 'sideButtonUpShort',
            'sideButtonUpLong', 'sideButtonDownShort', 'sideButtonDownLong']
    if 'sideButtonChordAction' in settings:
        keys.append('sideButtonChordAction')
    if args.home:
        keys += ['homeButtonTapAction', 'homeButtonDoubleTapAction', 'homeButtonLongPressAction']
    selected = {}
    original = {}
    for key in keys:
        assert key in settings, f'Missing web shortcut: {key}'
        options = settings[key]['options']
        assert options.count('Rotate Page 180°') == 1, f'Missing or duplicate rotation: {key}'
        selected[key] = options.index('Rotate Page 180°')
        original[key] = settings[key]['value']
    # These large rows used to be silently discarded by the 512-byte serializer.
    assert any(len(json.dumps(settings[key]).encode()) > 512 for key in keys)
    try:
        save(selected)
        reloaded = read()
        for key in keys:
            setting = reloaded[key]
            assert setting['value'] == selected[key], f'Save/reload failed: {key}'
            assert setting['options'][setting['value']] == 'Rotate Page 180°'
        print(f'PASS: {len(keys)} web shortcut catalogs and save/reload; oversized rows preserved')
    finally:
        save(original)


if __name__ == '__main__':
    main()
