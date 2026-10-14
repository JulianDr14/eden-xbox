# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
from pathlib import Path
import tempfile

spec = importlib.util.spec_from_file_location(
    "probe", Path(__file__).resolve().parents[1] / "analyze-material-probe.py")
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)
with tempfile.TemporaryDirectory() as temporary:
    index = Path(temporary) / "capture.csv"
    header = "draw,target,dxgi,x,y,width,height,address,vs,ps,offset,subresource\n"
    index.write_text(header + "1,0,28,0,0,2,1,1234,a,b,0,0\n"
                     "2,0,28,0,0,2,1,1234,a,b,8,0\n"
                     "3,0,28,0,0,2,1,1234,a,b,16,1\n"
                     "4,0,28,0,0,2,1,1234,a,b,24,0\n")
    index.with_suffix(".bin").write_bytes(bytes(8) + bytes(7) + b"\xff" + bytes(16))
    records = probe.analyze(index)["records"]
    assert [r["changed_texels"] for r in records] == [None, 1, None, 1]
    assert records[-1]["previous_draw"] == 2
    for data in (bytes(31), bytes(33)):
        index.with_suffix(".bin").write_bytes(data)
        try:
            probe.analyze(index)
        except ValueError:
            pass
        else:
            raise AssertionError("accepted truncated/unindexed bytes")
    index.write_text(header + "1,0,28,0,0,65,1,1234,a,b,0,0\n")
    try:
        probe.analyze(index)
    except ValueError:
        pass
    else:
        raise AssertionError("accepted invalid extent")
print("PASS native ROI parser: byte changes, mip separation, truncation, trailing bytes, bounds")
