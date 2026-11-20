#!/usr/bin/env python3
"""Runs a blind series of dot tests through the hub.

    python3 series.py <results_dir> <log_file> [uart_port]

Applies the hub setting for run 1, then after every saved 'hub-*.json' result applies the
next setting and notes which setting each run used (in <log_file>, as JSON lines).
Settings (hub console keys): a = 8 ms no resampling, b = 0 ms, c = 4 ms, d = 8 ms + resampling, e = 4 ms + resampling.
"""
import glob, json, os, sys, time
import serial

results, logf = sys.argv[1], sys.argv[2]
port = sys.argv[3] if len(sys.argv) > 3 else "/dev/cu.usbserial-31120"
order = ["e", "b", "a", "d", "c", "a"]           # shuffled; a is repeated to show run-to-run noise
s = serial.Serial(port, 115200, timeout=0.2)
def apply(k):
    s.write(k.encode()); time.sleep(0.3)
seen = set(glob.glob(os.path.join(results, "hub-*.json")))
i = 0
apply(order[0])
log = open(logf, "a", buffering=1)
log.write(json.dumps({"event": "start", "setting": order[0], "run": 1, "t": time.time()}) + "\n")
print("run 1 of", len(order), "ready", flush=True)
while i < len(order):
    time.sleep(0.5)
    new = [f for f in glob.glob(os.path.join(results, "hub-*.json")) if f not in seen]
    for f in sorted(new):
        seen.add(f)
        log.write(json.dumps({"event": "saved", "file": os.path.basename(f), "setting": order[i], "run": i + 1}) + "\n")
        i += 1
        if i < len(order):
            apply(order[i])
            log.write(json.dumps({"event": "next", "setting": order[i], "run": i + 1, "t": time.time()}) + "\n")
            print(f"run {i} saved; run {i + 1} of {len(order)} ready", flush=True)
        else:
            print("series finished", flush=True)
            apply("a")
            break
