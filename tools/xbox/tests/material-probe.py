# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
import struct
import tempfile
from pathlib import Path

spec = importlib.util.spec_from_file_location('probe', Path(__file__).resolve().parents[1] / 'analyze-material-probe.py')
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)
assert probe.texel(bytes((255, 128, 0, 0)), 28) == (1, 128/255, 0, 0)
assert probe.texel(bytes((0, 128, 255, 64)), 91) == (1, 128/255, 0, 64/255)
assert probe.texel(struct.pack('<I', 1023 | (3 << 30)), 24) == (1, 0, 0, 1)
assert probe.texel(struct.pack('<I', (15 << 6) | ((15 << 6) << 11) | ((15 << 5) << 22)), 26) == (1, 1, 1, 1)
with tempfile.TemporaryDirectory() as directory:
    index = Path(directory) / 'probe.csv'
    index.write_text('draw,dxgi,x,y,width,height,address,vs,ps,offset\n'
                     '1,28,10,20,2,1,abc,1,2,0\n'
                     '2,28,10,20,2,1,abc,1,2,8\n'
                     '3,28,10,20,2,1,abc,1,2,16\n')
    black, red = bytes((0, 0, 0, 255)), bytes((255, 0, 0, 255))
    data = black * 2 + red + black + red + black
    index.with_suffix('.bin').write_bytes(data)
    tiles = probe.load_tiles(index)
    summary = probe.summarize(tiles, (10, 20), Path(directory))
    assert len(summary) == 2 and summary[1]['draw'] == 2 and summary[1]['changed_pixels'] == 1
    assert summary[1]['pixel_rgba_native'] == (1, 0, 0, 1)
    bmp = (Path(directory) / 'draw_0002_abc_rt0.bmp').read_bytes()
    assert len(bmp) == 62 and bmp[:2] == b'BM' and bmp[54:58] == bytes((0, 0, 255, 255))
    for invalid in (data[:-1], data + b'\0'):
        index.with_suffix('.bin').write_bytes(invalid)
        try:
            probe.load_tiles(index)
        except ValueError:
            pass
        else:
            raise AssertionError('Accepted incomplete/extra pixel data')
    index.write_text('draw,target,dxgi,x,y,width,height,address,vs,ps,offset\n'
                     '1,3,28,10,20,2,1,abc,1,2,0\n')
    index.with_suffix('.bin').write_bytes(black * 2)
    assert probe.load_tiles(index)[0]['target'] == 3
    index.write_text(index.read_text().replace('1,3,28', '1,8,28'))
    try:
        probe.load_tiles(index)
    except ValueError:
        pass
    else:
        raise AssertionError('Accepted invalid MRT slot')
print('PASS probe: native RGBA/BGRA/10-bit/float, changes, pixel position, BMP, truncated/trailing data')
