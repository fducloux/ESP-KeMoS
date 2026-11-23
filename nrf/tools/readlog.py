#!/usr/bin/env python3
"""Read the hub's RAM log (last ~150 events) over Bluetooth, no cable needed. Needs: pip install bleak.

    python readlog.py            print the log
    python readlog.py > hub.log  save it

The hub must be reachable: a computer already paired with it (run this on that computer), or while setup is open."""
import asyncio, sys
from bleak import BleakScanner, BleakClient
CH_LOG = "5a6f0004-6b76-4d48-8f2e-7d9c1b2a3c4d"

async def main():
    found = await BleakScanner.discover(timeout=10, return_adv=True)
    dev = next((d for a, (d, adv) in found.items() if (adv.local_name or d.name or "").startswith("8REC")), None)
    if not dev: sys.exit("hub not seen")
    async with BleakClient(dev, timeout=20) as c:
        pos = 0
        while True:
            await c.write_gatt_char(CH_LOG, str(pos).encode(), response=True)
            lines = bytes(await c.read_gatt_char(CH_LOG)).decode(errors="replace").splitlines()
            if not lines: break
            for l in lines: print(l.split("|", 1)[-1])
            pos = int(lines[-1].split("|", 1)[0]) + 1
asyncio.run(main())
