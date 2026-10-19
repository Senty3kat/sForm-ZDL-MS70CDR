#!/usr/bin/env python3
"""Build sForm.ZDL from sform.c + manifest_pedal.json (9-knob LFO into R).

Requires the TI C6000 compiler at the TI_ROOT path below (edit if yours
differs). Run from the repo root:  python src/custom/sform/build.py
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent.parent           # src/custom/sform/build.py -> repo root
sys.path.insert(0, str(ROOT / "build"))
sys.path.insert(0, str(ROOT / "src" / "airwindows" / "common"))

from linker import LinkerConfig, link, params_from_manifest  # noqa: E402
from manifest_params import write_param_header  # noqa: E402
from custom_covers import make_cover  # noqa: E402
from parameter_display import pedal_name  # noqa: E402
from pack_covers import SFORM_KNOBS  # noqa: E402
from screen_image import Canvas  # noqa: E402
sys.path.insert(0, str(HERE.parent / "xduck"))   # knob_icons.py lives in xduck/ in the fork
sys.path.insert(0, str(HERE))                   # ...and next to this file in the sForm repo
from knob_icons import sform_wave_icons  # noqa: E402

KNOB_PIC, KNOB_FRAMES = sform_wave_icons()

TI_ROOT = Path(r"C:\ti\ccs2101\ccs\tools\compiler\ti-cgt-c6000_8.5.0.LTS")
CL6X = TI_ROOT / "bin" / "cl6x"

CFLAGS = [
    "--c99",
    "--opt_level=2",
    "-mv6740",
    "--abi=eabi",
    "--mem_model:data=far",
    f"--include_path={TI_ROOT}/include",
]


def write_em_png(path: Path, version: str) -> None:
    """Effect Manager sidecar, same format as XDuck's: 128x96 RGBA, black ink
    on transparent; the reference art at 128x87, the version underneath."""
    import struct
    import zlib

    w, h = 128, 96
    ink = [[0] * w for _ in range(h)]
    art = json.loads((HERE / "sform_art.json").read_text(encoding="utf-8"))
    for y, row in enumerate(art["em"]):
        for x, ch in enumerate(row):
            if ch == "1":
                ink[y][x] = 1
    text = version.upper()
    x0 = w - 1 - (len(text) * 4 - 1)
    for i, ch in enumerate(text):
        for ry, bits in enumerate(Canvas._FONT.get(ch, Canvas._FONT[" "])):
            for rx, b in enumerate(bits):
                if b == "1":
                    ink[90 + ry][x0 + i * 4 + rx] = 1
    raw = b"".join(
        b"\x00" + b"".join(b"\x00\x00\x00\xff" if v else b"\xff\xff\xff\x00" for v in row)
        for row in ink)

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    path.write_bytes(png)


def main() -> None:
    manifest = json.loads((HERE / "manifest_pedal.json").read_text())
    write_param_header(manifest, HERE / "sform_params.h", "SFORM")

    src_c = HERE / "sform.c"
    out_dir = Path(r"C:\zoom\zdlnew")
    out_dir.mkdir(exist_ok=True)

    effect_name = manifest["effect_name"]
    audio_func = manifest["audio_func_name"]
    obj = HERE / f"{effect_name.lower()}.obj"
    out_zdl = out_dir / f"{effect_name}.ZDL"

    print(f"[sform] compiling {src_c.name} -> {obj.name}")
    subprocess.run(
        [
            str(CL6X),
            *CFLAGS,
            f"--define=SFORM_AUDIO_FUNC={audio_func}",
            "-c",
            str(src_c),
            f"--output_file={obj}",
        ],
        check=True,
        cwd=HERE,
    )

    for junk in ("compiler.opt", "linker.cmd"):
        p = HERE / junk
        if p.exists():
            p.unlink()

    cfg = LinkerConfig(
        materialize_init=True,
        effect_name=effect_name,
        screen_image=make_cover(effect_name,
                                [pedal_name(effect_name, p["name"]) for p in manifest["params"]]),
        audio_func_name=audio_func,
        gid=manifest["gid"],
        fxid=manifest["fxid"],
        params=params_from_manifest(manifest["params"]),
        obj_path=obj,
        output_path=out_zdl,
        fxid_version=manifest.get("fxid_version", "1.00").encode("ascii"),
        flags_byte=manifest.get("flags_byte", 0x01),
        audio_nop=manifest.get("audio_nop", False),
        dsp_cost=manifest.get("dsp_cost", 20.0),
        knob_positions=SFORM_KNOBS,
        allow_fewer_knob_boxes=True,
        knob_pic=KNOB_PIC,                     # Wave shape icon
        knob_frames=KNOB_FRAMES,
        use_object_edit_handlers=False,
        synthesize_linesel_edit_handlers=True,
        synth_edit_start_index=2,
        knob3_blob_path="/tmp/__nonexistent__",
    )
    link(cfg)
    write_em_png(out_zdl.with_suffix(".png"), f"V{manifest['fxid_version']}")

    print(f"\n[sform] done -> {out_zdl}")


if __name__ == "__main__":
    main()
