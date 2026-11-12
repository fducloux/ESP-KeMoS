#!/usr/bin/env python3
"""Pack the pad firmware (bootloader, partition table, app) into padfw.blob for the hub's 'padfw' partition.

Run after building experiments/pad:  python tools/make_padfw.py
Write it to the hub:  python -m esptool --port <hub port> write_flash 0x190000 padfw.blob
The pad's NVS (pairing) is not in the blob, so flashing the pad from the hub keeps its pairing.
Format: "PFW1", then per part: u32 flash offset, u32 length, data; a part with length 0 ends it.
"""
import json, struct, sys, pathlib

root = pathlib.Path(__file__).resolve().parent.parent
build = root / "experiments" / "pad" / "build"
args = json.load(open(build / "flasher_args.json"))
out = bytearray(b"PFW1")
for off, name in sorted(args["flash_files"].items(), key=lambda kv: int(kv[0], 16)):
    data = (build / name).read_bytes()
    data += b"\xff" * (-len(data) % 4)
    out += struct.pack("<II", int(off, 16), len(data)) + data
    print(f"{off:>8}  {len(data):7d}  {name}")
out += struct.pack("<II", 0, 0)
dest = root / "padfw.blob"
dest.write_bytes(out)
print(f"{dest}: {len(out)} bytes (partition holds {0x100000})")
if len(out) > 0x100000:
    sys.exit("too big for the padfw partition")
