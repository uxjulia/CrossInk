#!/usr/bin/env python3
"""Exercise chapter shortcuts through real simulator input and persisted book progress.

Build simulator or x4-pro-simulator first. Home adapter events are exercised by
the built-in smoke test; they do not use the scripted HAL button input here.
"""
import argparse
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]

def epub(path):
    with zipfile.ZipFile(path, 'w') as z:
        z.writestr('mimetype', 'application/epub+zip')
        z.writestr('META-INF/container.xml', '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>')
        manifest = '<item id="toc" href="toc.ncx" media-type="application/x-dtbncx+xml"/>'
        manifest += ''.join(f'<item id="c{i}" href="c{i}.xhtml" media-type="application/xhtml+xml"/>' for i in range(3))
        spine = ''.join(f'<itemref idref="c{i}"/>' for i in range(3))
        z.writestr('content.opf', f'<package version="2.0" unique-identifier="id" xmlns="http://www.idpf.org/2007/opf"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">chapter-test</dc:identifier><dc:title>Chapter shortcut test</dc:title><dc:language>en</dc:language></metadata><manifest>{manifest}</manifest><spine toc="toc">{spine}</spine></package>')
        points = ''.join(f'<navPoint id="n{i}" playOrder="{i+1}"><navLabel><text>Chapter {i+1}</text></navLabel><content src="c{i}.xhtml"/></navPoint>' for i in range(3))
        z.writestr('toc.ncx', f'<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1"><head><meta name="dtb:uid" content="chapter-test"/></head><docTitle><text>Shortcut test</text></docTitle><navMap>{points}</navMap></ncx>')
        for i in range(3):
            text = ''.join(f'<p>Chapter {i+1}, paragraph {n}. This fixture verifies the persisted chapter position after shortcut selection and cancellation.</p>' for n in range(30))
            z.writestr(f'c{i}.xhtml', f'<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Chapter {i+1}</title></head><body><h1>Chapter {i+1}</h1>{text}</body></html>')

def xtc(path, chapters=True):
    width, height, pages = 480, 800, 6
    chapter_data = b''
    if chapters:
        for i in range(3):
            row = bytearray(96)
            title = f'Chapter {i+1}'.encode()
            row[:len(title)] = title
            struct.pack_into('<HH', row, 80, i*2+1, i*2+2)
            chapter_data += row
    table_offset = 56 + len(chapter_data)
    data_offset = table_offset + pages*16
    bitmap = bytes([255]) * (width*height//8)
    page = struct.pack('<IHHBBIQ', 0x00475458, width, height, 0, 0, len(bitmap), 0) + bitmap
    header = struct.pack('<IBBHBBBBIQQQQII', 0x00435458, 1, 0, pages, 0, 0, 0, int(chapters), 1, 0, table_offset, data_offset, 0, 56 if chapters else 0, 0)
    table = b''.join(struct.pack('<QIHH', data_offset+i*len(page),len(page),width,height) for i in range(pages))
    path.write_bytes(header+chapter_data+table+page*pages)

CASES = {
    'short-power': ({'shortPwrBtn':38}, [(0,'POWER')]),
    'long-power': ({'longPwrBtn':38}, [(0,'POWER:850')]),
    'long-menu': ({'longPressMenuAction':27}, [(0,'ENTER:850')]),
    'long-back': ({'longPressBackAction':27}, [(0,'BACK:1100')]),
    'side-short': ({'sideButtonUpShort':38}, [(0,'UP')]),
    'front-left-long': ({'frontButtonLeftLong':38}, [(0,'LEFT:850')]),
    'front-right-long': ({'frontButtonRightLong':38}, [(0,'RIGHT:850')]),
    'side-long': ({'sideButtonUpLong':38}, [(0,'UP:850')]),
    'power-chord': ({'powerChordAction':34}, [(0,'POWER:350'),(20,'UP:280')]),
    'side-chord': ({'sideButtonChordAction':34}, [(0,'UP:350'),(20,'DOWN:280')]),
    'quick-actions': ({'shortPwrBtn':27,'quickActionSlots':[38,0,0,0,0,0]}, [(0,'POWER'),(450,'ENTER')]),
    'edge': ({'leftEdgeUp':11}, [(0,'SWIPE:0.01,0.75,0.01,0.25,400')]),
}

def run(env, kind, case, mode='select', program=None, logs=None):
    values, trigger = CASES[case]
    name = f'{env}-{kind}-{case}-{mode}'
    with tempfile.TemporaryDirectory(prefix='chapter-runtime-') as tmp:
        work=Path(tmp)
        fs=work/'fs_'; state=fs/'.crosspoint'; state.mkdir(parents=True)
        suffix = 'xtc' if kind=='xtc-empty' else kind
        book=fs/f'book.{suffix}'
        if kind=='epub': epub(book)
        elif kind.startswith('xtc'): xtc(book,kind!='xtc-empty')
        else: book.write_text('Text reader has no chapter list.\n'*200)
        settings={'shortPwrBtn':0,'longPwrBtn':0,'longPressMenuAction':0,'longPressBackAction':0,'showBootScreen':False}
        settings.update(values)
        (state/'crossink-settings.json').write_text(json.dumps(settings))
        (state/'state.json').write_text(json.dumps({'openEpubPath':f'/book.{suffix}','lastSleepFromReader':True,'showBootScreen':False}))
        script=[]
        for base in (2500,4700):
            script.extend(f'{base+offset}:{action}' for offset,action in trigger)
        if kind in ('epub', 'xtc'):
            script += ['3900:BACK']
            if mode=='select': script += ['6000:DOWN','6400:ENTER']
            else: script += ['6400:BACK']
        script += ['7800:BACK','9000:QUIT']
        e=os.environ.copy()
        for key in list(e):
            if key.startswith(('CROSSPOINT_SIM_','CROSSINK_SIMULATOR_SMOKE')): del e[key]
        e.update(SDL_VIDEODRIVER='dummy',CROSSPOINT_SIM_WAKE_REASON='power',CROSSPOINT_SIM_INPUT_SCRIPT=';'.join(script))
        p=subprocess.run([str(program or ROOT/'.pio'/'build'/env/'program')],cwd=work,env=e,text=True,capture_output=True,timeout=20)
        out=p.stdout+p.stderr
        if logs:
            logs.mkdir(parents=True, exist_ok=True)
            (logs/f'{name}.log').write_text(out)
        assert p.returncode==0, (name,p.returncode,out[-2000:])
        assert not re.search(r'Segmentation fault|Assertion failed|std::bad_alloc|uncaught exception',out),name
        reader={'epub':'EpubReader','txt':'TxtReader','xtc':'XtcReader','xtc-empty':'XtcReader'}[kind]
        chapter=reader+'ChapterSelection'
        entries=[(int(t),a) for t,a in re.findall(r'\[(\d+)\].*Entering activity: (\w+)',out)]
        opened=[t for t,a in entries if a==chapter]
        if kind in ('epub','xtc'):
            assert len(opened)==2,(name,entries)
            assert opened[0]<3900 and 4700<=opened[1]<6400,(name,entries)
            progress=list(state.glob(('epub_' if kind=='epub' else 'xtc_')+'*/progress.bin'))
            assert len(progress)==1,(name,progress)
            data=progress[0].read_bytes()
            actual=struct.unpack_from('<HH' if kind=='epub' else '<I',data)
            expected=(1,0) if kind=='epub' and mode=='select' else ((2,) if kind=='xtc' and mode=='select' else ((0,0) if kind=='epub' else (0,)))
            assert actual==expected,(name,actual,expected)
            assert any(a=='Home' and t>=7800 for t,a in entries),(name,entries)
        else:
            assert not opened,(name,entries)
            assert not any(a=='Home' and t<7800 for t,a in entries),(name,entries)
        print('PASS',name,flush=True)

if __name__ == '__main__':
    from concurrent.futures import ThreadPoolExecutor, as_completed
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--env', default='simulator')
    p.add_argument('--program', type=Path)
    p.add_argument('--logs', type=Path)
    p.add_argument('--cases', nargs='+', choices=CASES, default=[case for case in CASES if case != 'edge'])
    p.add_argument('--kinds', nargs='+', choices=['epub', 'xtc', 'txt', 'xtc-empty'], default=['epub', 'xtc'])
    p.add_argument('--mode', choices=['select', 'cancel'], default='select')
    p.add_argument('--jobs', type=int, default=1)
    a = p.parse_args()
    program = (a.program or ROOT/'.pio'/'build'/a.env/'program').resolve()
    if not program.is_file(): p.error(f'Simulator program not found: {program}')
    failures = []
    with ThreadPoolExecutor(max_workers=a.jobs) as pool:
        futures = {pool.submit(run, a.env, kind, case, a.mode, program, a.logs): (kind, case)
                   for kind in a.kinds for case in a.cases}
        for future in as_completed(futures):
            try: future.result()
            except Exception as error:
                failures.append((futures[future], str(error)))
    if failures:
        for case, error in failures: print('FAIL', case, error)
        raise SystemExit(1)
