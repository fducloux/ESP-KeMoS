#!/usr/bin/env python3
"""Look at the left or right half of the monitor and the hub switches to that computer (prototype).

Runs on the Mac (camera + MediaPipe), talks to the hub over Bluetooth through its config service.
Needs: opencv-python-headless numpy mediapipe bleak, and face_landmarker.task next to this file
(https://storage.googleapis.com/mediapipe-models/face_landmarker/face_landmarker/float16/1/face_landmarker.task).
Run it from Terminal.app (camera and Bluetooth permission): python zoneswitch.py [--left 1] [--right 2] [--dry]

0. Open http://127.0.0.1:8770/ in Chrome on the monitor and click once for full screen: a green dot (point your nose at it)
   and a red ring (look at it with your eyes) show what the spoken instructions ask for.
1. Needs the three seat tours once (--three). Each run then starts with 4 s of "look at the middle"; the head model (distance x head turn)
   decides left or right of the middle, so it works at any seat distance. (--calibrate: the old calibration, look at each half for 6 s.)
2. Live: a side must hold for --dwell seconds and the keyboard must have been idle for --idle ms before the hub is told
   "switch N". Ctrl+C stops it.
"""
import argparse, asyncio, glob, http.server, json, math, os, subprocess, threading, time
import cv2, numpy as np
import mediapipe as mp
from mediapipe.tasks import python as mpp
from mediapipe.tasks.python import vision
from bleak import BleakScanner, BleakClient

ap = argparse.ArgumentParser()
ap.add_argument("--left", type=int, default=1, help="computer (position) shown on the left half")
ap.add_argument("--right", type=int, default=2, help="computer (position) shown on the right half")
ap.add_argument("--dwell", type=float, default=0.5, help="seconds a side must hold before switching")
ap.add_argument("--idle", type=int, default=800, help="keyboard idle time (ms) required before switching")
ap.add_argument("--enter", type=float, default=0.45, help="score (0 = middle, 1 = calibrated side) needed to count as looking at a side")
ap.add_argument("--camera", type=int, default=0)
ap.add_argument("--tour", action="store_true", help="instead of switching, run a longer guided tour (head and eyes separately) and save the numbers to tour.json")
ap.add_argument("--minutes", type=float, default=0, help="stop by itself after this many minutes of live switching (0 = run until Ctrl+C)")
ap.add_argument("--stay", type=float, default=5.0, help="after a switch, stay on that computer at least this many seconds before another switch (time to use the keyboard and mouse)")
ap.add_argument("--prefix", default="r", help="name of a tour set, e.g. ng for a set recorded without glasses: --three --prefix ng saves tour_ng_near/mid/far.json, and --check / --dot / live use that set when given the same --prefix")
ap.add_argument("--eyes", action="store_true", help="live switching: also use the eyes (the blue dot). The head and seat position alone (the white ring) is the default: it measured more accurate and unbiased")
ap.add_argument("--margin", type=float, default=0.06, help="dead zone around the middle of the screen, as a fraction of its width (0.06 = 6 percent each side): the head must point this far left or right of the middle to count as a side")
ap.add_argument("--calibrate", action="store_true", help="use the old calibration (look at each half for 6 s) instead of the tour_*.json head model")
ap.add_argument("--check", action="store_true", help="accuracy check: 9 dots from 10 to 90 percent of the screen width, a few seconds each, then it ends and prints the error in pixels (uses the tour_*.json files)")
ap.add_argument("--tag", default="tour", help="name of this tour: it is saved as tour_<tag>.json (do one sitting close, one in the middle, one leaning back)")
ap.add_argument("--three", action="store_true", help="short tour at three seat positions in a row: close, middle, far (saves tour_near/mid/far.json)")
ap.add_argument("--secs", type=float, default=6.0, help="seconds of measuring per pose in the tours (shorter = faster tour, a bit noisier)")
ap.add_argument("--real", action="store_true", help="tour with realistic poses only (head a quarter or three quarters across, eyes to the middle or the far end; small glances; both together), no head-or-eyes-only extremes")
ap.add_argument("--five", action="store_true", help="tour at five seat positions: centred, chair rolled left, chair rolled right, leaning in, leaning back (use with --prefix and usually --real)")
ap.add_argument("--full", action="store_true", help="with --three: all 9 poses at each seat (adds the head-only and eyes-only conflict poses the eye model needs), about 4.5 minutes")
ap.add_argument("--short", action="store_true", help="tour with the 7 natural poses only (about 60 s)")
ap.add_argument("--dot", action="store_true", help="show where the camera thinks you look as live dots on the page (uses tour.json from a --tour run); no hub, no switching")
ap.add_argument("--port", type=int, default=8770, help="port of the dots page")
ap.add_argument("--dry", action="store_true", help="do not talk to the hub, only print what would happen")
ap.add_argument("--say", action="store_true", help="speak 'left' / 'right' when switching")
args = ap.parse_args()

here = os.path.dirname(os.path.abspath(__file__))
SVC_CMD = "5a6f0003-6b76-4d48-8f2e-7d9c1b2a3c4d"; SVC_ST = "5a6f0002-6b76-4d48-8f2e-7d9c1b2a3c4d"


def say(t): subprocess.Popen(["say", t])


# ---------------------------------------------------------------- hub (Bluetooth, in its own thread)
class Hub:
    def __init__(self):
        self.loop = asyncio.new_event_loop(); self.client = None; self.status = {}; self.ready = threading.Event()
        threading.Thread(target=self.loop.run_forever, daemon=True).start()
        asyncio.run_coroutine_threadsafe(self.connect(), self.loop)

    async def connect(self):
        while True:
            try:
                found = await BleakScanner.discover(timeout=8, return_adv=True)
                dev = next((d for a, (d, adv) in found.items() if (adv.local_name or d.name or "").startswith("8REC")), None)
                if not dev: print("hub not seen, retrying"); continue
                self.client = BleakClient(dev, timeout=15); await self.client.connect(); self.ready.set(); print("hub connected")
                while self.client.is_connected: await asyncio.sleep(1)
                self.ready.clear(); print("hub link lost, reconnecting")
            except Exception as e:
                print("hub error:", e); self.ready.clear(); await asyncio.sleep(2)

    def read(self):
        if not self.ready.is_set(): return None
        try:
            v = asyncio.run_coroutine_threadsafe(self.client.read_gatt_char(SVC_ST), self.loop).result(5)
            self.status = json.loads(bytes(v).decode()); return self.status
        except Exception as e:
            print("status read failed:", e); return None

    def command(self, text):
        if not self.ready.is_set(): return False
        try:
            asyncio.run_coroutine_threadsafe(self.client.write_gatt_char(SVC_CMD, text.encode(), response=True), self.loop).result(5); return True
        except Exception as e:
            print("command failed:", e); return False


# ---------------------------------------------------------------- face features
opts = vision.FaceLandmarkerOptions(
    base_options=mpp.BaseOptions(model_asset_path=os.path.join(here, "face_landmarker.task")),
    running_mode=vision.RunningMode.VIDEO, num_faces=1, output_face_blendshapes=True, output_facial_transformation_matrixes=True)
lm = vision.FaceLandmarker.create_from_options(opts)
cap = cv2.VideoCapture(args.camera, cv2.CAP_AVFOUNDATION)
cap.set(cv2.CAP_PROP_FRAME_WIDTH, 1280); cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 720)
cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*"MJPG")); cap.set(cv2.CAP_PROP_FPS, 30)
t0 = time.time()


def features():
    """[head yaw (degrees), gaze x from the eyes (nan while an eye is closed), head x, head distance (about cm)] for one frame, or None (no face)."""
    ok, f = cap.read()
    if not ok: return None
    res = lm.detect_for_video(mp.Image(image_format=mp.ImageFormat.SRGB, data=cv2.cvtColor(f, cv2.COLOR_BGR2RGB)), int((time.time() - t0) * 1000))
    if not res.face_landmarks or not res.face_blendshapes or not len(res.facial_transformation_matrixes): return None
    b = {c.category_name: c.score for c in res.face_blendshapes[0]}
    blink = b.get("eyeBlinkLeft", 0) > 0.5 or b.get("eyeBlinkRight", 0) > 0.5        # a closed eye spoils the eye reading only, not the head pose
    R = np.array(res.facial_transformation_matrixes[0]).reshape(4, 4)[:3, :3]
    yaw = math.degrees(math.atan2(R[0, 2], R[2, 2]))
    gx = float("nan") if blink else (b["eyeLookOutLeft"] + b["eyeLookInRight"]) - (b["eyeLookInLeft"] + b["eyeLookOutRight"])
    T = np.array(res.facial_transformation_matrixes[0]).reshape(4, 4)[:3, 3]          # head position in camera space (about cm)
    pts = res.face_landmarks[0]; iris = float("nan")
    if len(pts) > 473 and not blink:      # where the iris sits between the eye corners, right eye minus left eye: about +0.2 looking one way, -0.2 the other
        def ratio(o, i, c): return (pts[c].x - pts[o].x) / ((pts[i].x - pts[o].x) or 1e-6)
        iris = ratio(33, 133, 468) - ratio(263, 362, 473)
    return np.array([yaw, gx, T[0], abs(T[2]), iris])


def record(seconds):
    rows = []; end = time.time() + seconds
    while time.time() < end:
        f = features()
        if f is not None: rows.append(f)
    return np.array(rows)


# ---------------------------------------------------------------- the dots page (served from here, so it always matches the voice)
state = {"text": "Starting…", "nose": 0.5, "eyes": 0.5, "until": 0, "mode": "idle", "zone": None, "score": 0.0}


class Page(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith("/state"):
            body, kind = json.dumps(state).encode(), "application/json"
        else:
            body, kind = open(os.path.join(here, "guide.html"), "rb").read(), "text/html; charset=utf-8"
        self.send_response(200); self.send_header("Content-Type", kind); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)

    def log_message(self, *a): pass


http.server.ThreadingHTTPServer.allow_reuse_address = True
threading.Thread(target=http.server.ThreadingHTTPServer(("127.0.0.1", args.port), Page).serve_forever, daemon=True).start()
print(f"dots page: http://127.0.0.1:{args.port}/  (open it in Chrome on the monitor, click once for full screen)")


def phase(text, nose, eyes, secs, settle=2.5):
    """Show the dots, say the instruction, give time to move, then record `secs` seconds of face features."""
    say(text); state.update(text=text, nose=nose, eyes=eyes, mode="settle", until=time.time() + settle); time.sleep(settle)
    state.update(mode="record", until=time.time() + secs); rows = record(secs); state.update(mode="idle")
    return rows


# ---------------------------------------------------------------- calibration
if args.tour or args.dot or args.check: args.dry = True
hub = None if args.dry else Hub()
state.update(text="Open this page full screen on the monitor, then sit as you normally do. Starting in 8 seconds…", until=time.time() + 8); time.sleep(8)
if args.three: args.tour = True; args.short = not args.full
if args.five: args.tour = True
if args.tour:
    TOUR = [("middle", "Point your nose and your eyes at the middle", 0.5, 0.5), ("far left", "Nose and eyes to the far left", 0.06, 0.06),
            ("far right", "Nose and eyes to the far right", 0.94, 0.94), ("eyes left", "Nose straight at the middle, eyes only to the far left", 0.5, 0.06),
            ("eyes right", "Nose straight at the middle, eyes only to the far right", 0.5, 0.94), ("head left", "Nose to the far left, eyes straight ahead at the middle", 0.06, 0.5),
            ("head right", "Nose to the far right, eyes straight ahead at the middle", 0.94, 0.5), ("left half", "Nose and eyes to the middle of the left half", 0.25, 0.25),
            ("right half", "Nose and eyes to the middle of the right half", 0.75, 0.75)]
    if args.real:
        TOUR = [("middle", "Point your nose and your eyes at the middle", 0.5, 0.5),
                ("left half", "Nose and eyes to the middle of the left half", 0.25, 0.25), ("right half", "Nose and eyes to the middle of the right half", 0.75, 0.75),
                ("far left", "Nose and eyes to the far left end", 0.06, 0.06), ("far right", "Nose and eyes to the far right end", 0.94, 0.94),
                ("nose L eyes mid", "Nose a quarter in from the left, eyes to the middle", 0.25, 0.5), ("nose R eyes mid", "Nose a quarter in from the right, eyes to the middle", 0.75, 0.5),
                ("nose L eyes far", "Nose a quarter in from the left, eyes to the far left end", 0.25, 0.06), ("nose R eyes far", "Nose a quarter in from the right, eyes to the far right end", 0.75, 0.94),
                ("glance L", "Nose at the middle, eyes a quarter toward the left", 0.5, 0.25), ("glance R", "Nose at the middle, eyes a quarter toward the right", 0.5, 0.75)]
    elif args.short: TOUR = [t for t in TOUR if t[0] in ("middle", "far left", "far right", "eyes left", "eyes right", "left half", "right half")]
    SEATS = [("near", "Lean in close to the screen, as close as you often get, and stay there")
             , ("mid", "Sit back to your normal working position and stay there")
             , ("far", "Lean back as far as you go when you work, and stay there")] if args.three else [(args.tag, None)]
    if args.five:
        SEATS = [("mid", "Sit centred in front of the monitor, in your normal working position, and stay there"),
                 ("left", "Roll your chair to the left, so you are centred in front of the left half, and stay there"),
                 ("right", "Roll your chair to the right, so you are centred in front of the right half, and stay there"),
                 ("near", "Back at the centre, lean in close to the screen, and stay there"),
                 ("far", "Back at the centre, lean back as far as you go when you work, and stay there")]
    for tag, seat_text in SEATS:
        if seat_text:
            say(seat_text); state.update(text=seat_text + ". Starting in 12 seconds.", mode="settle", nose=0.5, eyes=0.5, until=time.time() + 12); time.sleep(12)
        out = []
        for name, text, nx, ex in TOUR:
            rows = phase(text, nx, ex, args.secs)
            if len(rows): out.append({"name": name, "nose_x": nx, "eyes_x": ex, "frames": len(rows), "yaw": float(rows[:, 0].mean()), "yaw_sd": float(rows[:, 0].std()), "gaze": float(np.nanmean(rows[:, 1])), "gaze_sd": float(np.nanstd(rows[:, 1])), "tx": float(rows[:, 2].mean()), "tz": float(rows[:, 3].mean()), "tz_sd": float(rows[:, 3].std()), "iris": float(np.nanmean(rows[:, 4])), "iris_sd": float(np.nanstd(rows[:, 4]))})
            print(out[-1] if out and out[-1]["name"] == name else f"{name}: no face frames")
        fn = f"tour_{args.prefix}_{tag}.json" if args.prefix and (args.three or args.five) else f"tour_{tag}.json"
        json.dump(out, open(os.path.join(here, fn), "w"), indent=1); print(f"saved {fn}")
    state.update(text="All finished, thank you.", mode="idle"); say("Finished, thank you"); raise SystemExit(0)
model_rows = []
import re
for fn in sorted(glob.glob(os.path.join(here, "tour_*.json"))):
    base = os.path.basename(fn)
    if not re.fullmatch(rf"tour_{re.escape(args.prefix) + '_' if args.prefix else ''}(near|mid|far|left|right)\.json", base): continue      # one set at a time: with or without glasses
    t = json.load(open(fn))
    if t and "tz" in t[0]: model_rows += [dict(r, file=os.path.basename(fn)) for r in t]
if args.dot or args.check or (not args.tour and not args.calibrate and model_rows):       # the head model: used by --dot, --check and the live switching
    rows = model_rows
    if not rows: raise SystemExit("no tour_<tag>.json with head positions yet: run --three first")
    print(f"{len(rows)} tour points from {sorted(set(r['file'] for r in rows))}; head distance range {min(r['tz'] for r in rows):.0f}..{max(r['tz'] for r in rows):.0f}")

    # Measured on five seat positions (centred, chair rolled left and right, leaning in and out), each seat held out in turn:
    # screen position is linear in (head sideways position, head distance x head turn, head distance x iris position). 239 px mean error.
    def feat_h(f): return np.array([1.0, f[2], f[3] * f[0]])                         # head only: 1, sideways position, distance*yaw
    def feat_b(f): return np.array([1.0, f[2], f[3] * f[0], f[3] * f[4]])            # head + eyes: ... + distance*iris

    def fit_lin(pts, feat):
        X = np.array([feat([r["yaw"], r["gaze"], r["tx"], r["tz"], r["iris"]]) for r in pts]); y = np.array([r["eyes_x"] for r in pts])
        w = np.linalg.lstsq(X, y, rcond=None)[0]
        return w, float(np.mean(abs(X @ w - y)))

    good = [r for r in rows if r.get("frames", 0) >= 20 and "iris" in r and not math.isnan(r["iris"])]
    if len(good) < 10: raise SystemExit("these tours have no iris data: record them with --five --real --prefix r")
    wh, eh_fit = fit_lin(good, feat_h); wb, eb_fit = fit_lin(good, feat_b)
    print(f"white ring (head + sideways position): fit error {eh_fit * 5120:.0f} px | blue dot (+ eyes, the iris): fit error {eb_fit * 5120:.0f} px")
    mid = phase("First, look at the middle of the screen with your nose and your eyes", 0.5, 0.5, 4)
    if len(mid) < 10: raise SystemExit("no face frames")
    m = np.nanmean(mid, axis=0)
    off_h = 0.5 - feat_h(m) @ wh; off_b = 0.5 - feat_b(m) @ wb      # today's seat: shift so the middle is the middle
    def est_h(f): return float(feat_h(f) @ wh + off_h)
    def est_b(f): return float(feat_b(f) @ wb + off_b) if not math.isnan(f[4]) else est_h(f)     # an eye closed: fall back to the head only
    print(f"at the middle: head distance {m[3]:.0f}, x {m[2]:.1f}")
    if args.check:
        import random
        order = [10, 20, 30, 40, 50, 60, 70, 80, 90]; random.Random(7).shuffle(order); res = []
        for pct in order:
            x = pct / 100
            rr = phase("Look at the dot with your nose and your eyes", x, x, 3.5, settle=2.2)
            fr = [(est_h(r), est_b(r)) for r in rr]
            if not fr: print(f"{pct}%: no face frames"); continue
            eh_, eb_ = (float(np.median([v[0] for v in fr])), float(np.median([v[1] for v in fr])))
            res.append({"target_pct": pct, "ring_pct": round(eh_ * 100, 1), "dot_pct": round(eb_ * 100, 1), "ring_err_px": round((eh_ - x) * 5120), "dot_err_px": round((eb_ - x) * 5120)})
            print(f"target {pct:2d}%   white ring {eh_ * 100:5.1f}% ({(eh_ - x) * 5120:+5.0f} px)   blue dot {eb_ * 100:5.1f}% ({(eb_ - x) * 5120:+5.0f} px)")
        if res:
            print(f"\nmean error: white ring {np.mean([abs(r['ring_err_px']) for r in res]):.0f} px, blue dot {np.mean([abs(r['dot_err_px']) for r in res]):.0f} px (screen is 5120 px wide)")
            json.dump(res, open(os.path.join(here, "check.json"), "w"), indent=1)
        state.update(text="Check finished, thank you.", mode="idle"); say("Finished, thank you"); raise SystemExit(0)
    if not args.dot:                      # live switching with the head model: no calibration per session, works at any seat distance
        say("Live. Look at the left or the right half"); state.update(mode="live", text="Live: look at the left or the right half"); print(f"live: left half = computer {args.left}, right half = computer {args.right}. Ctrl+C to stop")
        ex = 0.5; zone = None; cand = None; cand_since = 0; last_switch = 0; shown = 0; frames = 0; start = time.time()
        logname = os.path.join(here, time.strftime("live_%Y%m%d_%H%M%S.csv")); logf = open(logname, "w")
        logf.write("t,yaw,gaze,head_x,distance,head_at_pct,candidate,zone,locked,hub_active,kbd_idle_ms,event\n"); print(f"logging every frame to {logname}")
        hub_active, kidle, last_poll = "", "", 0
        try:
            while True:
                f = features()
                if f is None: continue
                frames += 1
                now = time.time()
                if args.minutes and now - start > args.minutes * 60: say("Time is up, thank you"); raise SystemExit("time is up")
                ex = 0.6 * ex + 0.4 * max(0.0, min(1.0, est_b(f) if args.eyes else est_h(f)))      # where the head points, 0 = left edge, 1 = right edge
                side = "L" if ex < 0.5 - args.margin else "R" if ex > 0.5 + args.margin else None
                state.update(zone=zone, score=(0.5 - ex) / 0.5)
                if side != cand: cand = side; cand_since = now
                if hub and now - last_poll > 1.0:
                    last_poll = now; st = hub.read()
                    if st: hub_active, kidle = st["active"] + 1, st["kidle"]
                locked = now - last_switch < args.stay
                ev = ""
                if cand and cand != zone and now - cand_since >= args.dwell and not locked:
                    target = args.left if cand == "L" else args.right
                    if args.dry:
                        ev = f"would switch to {target}"; print(f"[dry] would switch to computer {target} (looking {'left' if cand == 'L' else 'right'})"); zone = cand; last_switch = now
                    else:
                        st = hub.read()
                        if st is None: ev = "hub not readable"
                        elif st["active"] == target - 1: zone = cand; ev = "already active"
                        elif st["kidle"] < args.idle: ev = f"waiting: typing (idle {st['kidle']} ms)"
                        elif hub.command(f"switch {target}"):
                            zone = cand; last_switch = now; hub_active = target; ev = f"switched to {target}"
                            print(f"-> computer {target} ({'left' if cand == 'L' else 'right'}); staying at least {args.stay:.0f} s")
                            if args.say: say("left" if cand == "L" else "right")
                logf.write(f"{now - start:.3f},{f[0]:.1f},{f[1]:.2f},{f[2]:.1f},{f[3]:.1f},{ex * 100:.1f},{cand or ''},{zone or ''},{int(locked)},{hub_active},{kidle},{ev}\n")
                if ev: logf.flush()
                if now - shown > 2:
                    logf.flush(); shown = now; print(f"head at {ex * 100:5.1f}%  zone {zone or '-'}  {'(staying)' if locked else ''}  distance {f[3]:.0f}  {frames / (now - start):.1f} fps")
        except KeyboardInterrupt:
            logf.close(); raise SystemExit("stopped")
    say("Now look at the marks on the screen")
    state.update(mode="dot", text="Look at the marks (percent of the screen width). White ring: head position and turn. Blue dot: head and eyes.", est_head=0.5, est_both=0.5)
    eh = eb = 0.5; shown = 0; frames = 0; start = time.time()
    try:
        while True:
            f = features()
            if f is None: continue
            frames += 1
            xh = est_h(f); xb = est_b(f)
            eh = 0.6 * eh + 0.4 * max(0.0, min(1.0, xh)); eb = 0.6 * eb + 0.4 * max(0.0, min(1.0, xb))
            state.update(est_head=eh, est_both=eb)
            if time.time() - shown > 2: shown = time.time(); print(f"white ring {eh * 100:5.1f}%  blue dot {eb * 100:5.1f}%  distance {f[3]:.0f}  {frames / (time.time() - start):.1f} fps")
    except KeyboardInterrupt:
        raise SystemExit("stopped")
L = phase("Point your nose at the green dot and look at the red ring, on the left half", 0.25, 0.25, 6)
Rr = phase("Now the same on the right half", 0.75, 0.75, 6)
if len(L) < 20 or len(Rr) < 20: raise SystemExit("not enough face frames during calibration (camera or light?)")
mL, mR = np.nanmean(L, axis=0)[:2], np.nanmean(Rr, axis=0)[:2]
mid = (mL + mR) / 2; span = mL - mR
use = np.abs(span) > np.array([4.0, 0.3])          # a feature counts only if the two sides differ clearly (yaw 4 degrees, gaze 0.3)
print(f"left  yaw {mL[0]:6.1f} gaze {mL[1]:5.2f} ({len(L)} frames)\nright yaw {mR[0]:6.1f} gaze {mR[1]:5.2f} ({len(Rr)} frames)\nusing yaw: {use[0]}, eyes: {use[1]}")
if not use.any(): raise SystemExit("the two sides look the same to the camera; calibration failed")


def score(f):                                       # +1 = the calibrated left side, -1 = the right side, 0 = the middle (None: nothing usable)
    f = np.asarray(f[:2], dtype=float); ok = use & ~np.isnan(f)
    return float(np.mean(2 * (f[ok] - mid[ok]) / span[ok])) if ok.any() else None


say("Calibrated. Live now"); print("live: Ctrl+C to stop"); state.update(mode="live", text="Live: look at the left or the right half")
ema = 0.0; zone = None; cand = None; cand_since = 0; last_switch = 0; shown = 0; frames = 0; start = time.time()
try:
    while True:
        f = features()
        if f is None: continue
        frames += 1
        sc = score(f)
        if sc is None: continue
        ema = 0.7 * ema + 0.3 * max(-1.5, min(1.5, sc))
        side = "L" if ema > args.enter else "R" if ema < -args.enter else None
        state.update(zone=zone, score=ema)
        if side != cand: cand = side; cand_since = time.time()
        if cand and cand != zone and time.time() - cand_since >= args.dwell and time.time() - last_switch > 1.0:
            target = args.left if cand == "L" else args.right
            if args.dry:
                print(f"[dry] would switch to computer {target} (looking {'left' if cand == 'L' else 'right'})"); zone = cand; last_switch = time.time()
            else:
                st = hub.read()
                if st is None: continue
                if st["active"] == target - 1: zone = cand
                elif st["kidle"] < args.idle: pass                      # typing: wait
                elif hub.command(f"switch {target}"):
                    zone = cand; last_switch = time.time(); print(f"-> computer {target} ({'left' if cand == 'L' else 'right'})")
                    if args.say: say("left" if cand == "L" else "right")
        if time.time() - shown > 2:
            shown = time.time(); print(f"score {ema:+.2f}  zone {zone or '-'}  {frames / (time.time() - start):.1f} fps")
except KeyboardInterrupt:
    print("stopped")
