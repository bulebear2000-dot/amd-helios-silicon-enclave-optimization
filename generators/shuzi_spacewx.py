#!/usr/bin/env python3
"""
Project Shuzi SPACEWX v8.5-EXTREME - space-weather-to-bit-flip fault generator + rescue-path harness
Python 3.9+, standard library only, single file.

IDEA
  Start at the Sun and follow the physical cause chain down to the bytes your engine reads:

    solar wind (speed, density, Bz)  ->  E-field  ->  geomagnetic storm  G1..G5
    flare (X-ray)                    ->  HF/L-band blackout              R1..R5
    solar energetic protons          ->  radiation level                 S1..S5
                                          |
      link attenuation + scintillation fades, GNSS time degradation (jitter, holdover drift,
      step on re-acquire), blackouts with buffered backlog flush (reordered, duplicated,
      overflow), packet loss / duplication / reordering, single-bit and multi-bit upsets,
      stuck-at bytes, sensor saturation, latch-up / brownout reboots with RTC reset to 2000,
      truncated writes, zero-filled sectors
                                          |
      plus classic time and storage faults: leap second, NTP step, GPS-week rollover,
      epoch edge values (2^31, 2^32, 2^53, int64 max), duplicated sector, idle gap,
      same-millisecond bursts, LF/CRLF switch, attaching to a stream mid-line

  Records keep the 6-column schema   ts,c1,c2,c3,STATE,flag   (c3 ~ dBm).
  Rescue signals (DISTRESS,1) are planted all the time and are NEVER dropped or corrupted by the
  generator; the harness checks that your engine still forwards them.  Every planted rescue signal
  carries a class that says what the environment did to it:

     A  clean            exact timestamp, on time, nominal signal level
     B  late             held in a blackout buffer and delivered afterwards (old timestamp)
     C  clock-faulty     GNSS jitter / holdover drift / step / RTC reset (timestamp is wrong)
     D  faded            signal level degraded by >= 6 dB, otherwise well formed
  (priority C > B > D > A).  Class A is the hard requirement; B, C, D show whether your engine
  keeps forwarding rescue signals that arrive late, with a wrong clock, or weak.
  A small share of rescue signals are "SEU victims": a bit flip is injected into the payload
  (never the timestamp).  They are best-effort and reported separately.

QUICK START
  python3 shuzi_spacewx.py --selftest
  python3 shuzi_spacewx.py --total 5000000 --run "./your_engine"
  python3 shuzi_spacewx.py --total 5000000 --campaign extreme --timeline sw.csv --stdout | ./your_engine

ENGINE CONTRACT for --run
  Engine reads the CSV stream on stdin and prints the first column (timestamp) of every record it
  forwards to the rescue path, one per line, on stdout (or --engine-out FILE).
  Exit codes: 0 pass | 1 rescue loss in a required class | 3 engine crashed / stalled / hung.
  --require A,B,C,D chooses which classes must be perfect (default: A).

CAMPAIGNS (--campaign)
  campaign (default: G1..G5 spread over the stream) | quiet | moderate (G2) | severe (G4) |
  extreme (G5S5R5) | explicit list such as  G3,G1S2R1,G5S4R3   (S and R default to G-1)
  Quiet lead-in and tail are kept (10% each) so class-A traffic always exists.

OUTPUT FILES (all optional)
  --timeline FILE   solar-wind / index series per block (speed, density, Bz, E, Kp, G, S, proton
                    flux, R, X-ray flux)
  --truth FILE      every injected fault: type, first/last record, detail
  --must FILE       planted rescue signals: record, class, ts
  --coverage FILE   counters per fault type
  --file FILE       write the stream to a file (prints its SHA-256)

DETERMINISM  Same seed and parameters give byte-identical output.
SAFETY  Inert data only: no network access, no executable payloads.  Scale values are indicative
  (NOAA SWPC G/S/R scales); this is a test-data generator, not a space-weather forecast.
"""
import argparse
import hashlib
import heapq
import json
import math
import os
import random
import re
import sys
import threading
import time
from collections import Counter, deque

if sys.version_info < (3, 9):
    sys.exit("shuzi_spacewx.py needs Python 3.9+")

VERSION = "8.5-EXTREME"
BASE_TS = 1731500000000
RTC_BASE = 946684800000                 # 2000-01-01T00:00:00Z: typical RTC power-on default
GPS_WEEK_MS = 604800000 * 1024          # GPS week-number rollover period
STATES = [b"STANDBY", b"OK", b"PRANK", b"DISTRESS"]
EDGE_TS = [2147483647, 2147483648, 4294967295, 4294967296, 9007199254740992, 9007199254740993,
           9223372036854775807, 253402300799000, 0, 946684800000, 1731500000, -1]
RAILS = (65535.0, 4095.0, -32768.0)     # saturated sensor values
FRAG_SIZES = (1, 2, 3, 5, 8, 13, 64, 500, 4096, 65536)

# ----- level tables, index = level 0..5 (indicative values) ---------------------------------
LV_ATTEN = [0, 1, 3, 6, 12, 25]                        # dB extra attenuation by geomagnetic level G
LV_FADE_RATE = [0, 2, 5, 12, 25, 50]                   # scintillation fade bursts per 10k records
LV_FADE_DEPTH = [0, 6, 10, 15, 25, 35]                 # dB
LV_SIGMA = [0, 2, 10, 60, 400, 3000]                   # GNSS timestamp jitter sigma, ms
LV_HOLD = [0.0, 0.0, 0.0, 0.002, 0.01, 0.05]           # holdover drift, ms per record while G >= 3
LV_LOSS = [0.0, 0.0002, 0.001, 0.005, 0.02, 0.08]      # record loss (never rescue signals)
LV_DUP = [0.0, 0.0002, 0.001, 0.004, 0.012, 0.03]
LV_REORDER = [0.0, 0.0005, 0.002, 0.008, 0.02, 0.05]
LV_BROWN = [0.0, 0.0, 0.0, 0.05, 0.4, 2.0]             # brownout reboots per 10k records by G
LV_SEL = [0.0, 0.0, 0.0, 0.02, 0.1, 0.5]               # latch-up reboots per 10k records by S
LV_GBLACK_P = [0.0, 0.0, 0.0, 0.04, 0.12, 0.30]        # ionospheric blackout probability per block
LV_GBLACK_LEN = [0, 0, 0, 60, 300, 1500]               # records
LV_R_BLACK = [0, 30, 150, 800, 4000, 20000]            # flare blackout records by R level
LV_SEU = [2e-5, 1e-4, 4e-4, 1.5e-3, 6e-3, 2.5e-2]      # bit-flip probability per record by S level

# ----- thresholds (NOAA SWPC style scales; E-field thresholds are indicative) -----------------
G_E = [3.0, 6.0, 10.0, 16.0, 25.0]                     # mV/m  (E = v * max(0,-Bz) / 1000)
S_PFU = [10.0, 1e2, 1e3, 1e4, 1e5]                     # >10 MeV proton flux, pfu
R_XR = [1e-5, 5e-5, 1e-4, 1e-3, 2e-3]                  # GOES X-ray flux, W/m^2
PEAK_V = {1: 500.0, 2: 600.0, 3: 750.0, 4: 1000.0, 5: 1500.0}
PEAK_E = {1: 4.5, 2: 8.0, 3: 13.0, 4: 20.0, 5: 40.0}   # band centres so the requested G level is reached


def level(x, th):
    return sum(1 for t in th if x >= t)


# =============================================================================================
# solar wind model
# =============================================================================================
def parse_campaign(spec):
    spec = (spec or "campaign").strip()
    if spec == "campaign":
        spec = "G1,G2,G3,G4,G5"
    elif spec == "quiet":
        return []
    elif spec == "moderate":
        spec = "G2"
    elif spec == "severe":
        spec = "G4"
    elif spec == "extreme":
        spec = "G5S5R5"
    out = []
    for tok in spec.split(","):
        m = re.fullmatch(r"G([1-5])(?:S([0-5]))?(?:R([0-5]))?", tok.strip().upper())
        if not m:
            raise SystemExit("bad --campaign token %r (use e.g. G3, G4S2R1, quiet, extreme)" % tok)
        g = int(m.group(1))
        s = int(m.group(2)) if m.group(2) is not None else max(0, g - 1)
        r = int(m.group(3)) if m.group(3) is not None else max(0, g - 1)
        out.append((g, s, r))
    return out


class SolarWind:
    def __init__(self, seed, nb, storms):
        rng = random.Random(seed ^ 0x50141)
        self.nb = nb
        self.v = [400.0 + 50.0 * math.sin(b / 37.0) + rng.gauss(0, 12) for b in range(nb)]
        self.n = [max(1.0, 5.0 + rng.gauss(0, 0.8)) for _ in range(nb)]
        self.bz = [max(-4.5, min(4.5, rng.gauss(0, 1.8))) for _ in range(nb)]
        self.pfu = [0.2 + abs(rng.gauss(0, 0.1)) for _ in range(nb)]
        self.xr = [10 ** rng.uniform(-8, -6.5) for _ in range(nb)]
        for (b0, g, s, r) in storms:
            vp = PEAK_V[g] * rng.uniform(0.95, 1.05)
            bzp = PEAK_E[g] * 1000.0 / (0.975 * vp)          # |Bz| so that E peaks inside the band of level g
            npk = rng.uniform(20, 60)
            if r > 0:                                         # flare: radio blackout first
                flux = R_XR[r - 1] * rng.uniform(1.0, 1.9)
                for t in range(1 + (r >= 3) + (r >= 5)):
                    if 0 <= b0 + t < nb:
                        self.xr[b0 + t] = max(self.xr[b0 + t], flux * (0.6 ** t))
            if s > 0:                                         # then the proton event
                peak = S_PFU[s - 1] * rng.uniform(1.0, 5.0)
                t = 0
                while b0 + 1 + t < nb and peak * math.exp(-t / 25.0) >= 0.5:
                    self.pfu[b0 + 1 + t] = max(self.pfu[b0 + 1 + t], peak * math.exp(-t / 25.0))
                    t += 1
            t0, ramp, main, rec = b0 + 4, 6, 10 + 4 * g, 40 + 10 * g   # finally the CME / geomagnetic storm
            tau = rec / 3.0
            t = 0
            while True:
                b = t0 + t
                if b >= nb:
                    break
                if t < ramp:
                    v, bz, nn = (0.65 + 0.35 * t / ramp) * vp, None, npk
                elif t < ramp + main:
                    u = (t - ramp) / main
                    v, bz, nn = vp * (1 - 0.05 * u), -bzp * math.sin(math.pi * u) + rng.gauss(0, 0.3), npk
                else:
                    fac = math.exp(-(t - ramp - main) / tau)
                    if fac < 0.02:
                        break
                    v, bz, nn = 400.0 + (0.9 * vp - 400.0) * fac, -bzp * 0.35 * fac, 5.0 + (npk - 5.0) * fac
                self.v[b] = max(self.v[b], v)
                self.n[b] = max(self.n[b], nn)
                if bz is not None:
                    self.bz[b] = min(self.bz[b], bz)
                t += 1
        self.E = [v * max(0.0, -bz) / 1000.0 for v, bz in zip(self.v, self.bz)]
        self.G = [level(e, G_E) for e in self.E]
        self.S = [level(p, S_PFU) for p in self.pfu]
        self.R = [level(x, R_XR) for x in self.xr]

    def kp(self, b):
        g = self.G[b]
        if g == 0:
            return min(4.0, self.E[b])
        hi = G_E[g] if g < 5 else 2 * G_E[4]
        return min(9.0, 4.0 + g + (self.E[b] - G_E[g - 1]) / (hi - G_E[g - 1]))

    def write_timeline(self, path, blk, upto):
        with open(path, "w") as f:
            f.write("block,first_source_record,speed_kms,density_cc,bz_nT,E_mVm,Kp,G,proton_pfu,S,xray_Wm2,R\n")
            for b in range(min(upto, self.nb)):
                f.write("%d,%d,%.1f,%.1f,%.2f,%.2f,%.1f,%d,%.2f,%d,%.3g,%d\n" % (
                    b, b * blk, self.v[b], self.n[b], self.bz[b], self.E[b], self.kp(b), self.G[b],
                    self.pfu[b], self.S[b], self.xr[b], self.R[b]))


# =============================================================================================
# simulator
# =============================================================================================
class Done(Exception):
    pass


class Rec:
    __slots__ = ("ts", "rest", "must", "cls", "victim", "raw")

    def __init__(self, ts, rest, must=False, cls=None, victim=False, raw=False):
        self.ts, self.rest, self.must, self.cls, self.victim, self.raw = ts, rest, must, cls, victim, raw


class Sim:
    def __init__(self, a, out):
        self.a, self.out = a, out
        self.rng = random.Random(a.seed)
        self.total = a.total
        self.blk = a.block or max(20, a.total // 1000)
        nb_plan = max(1, a.total // self.blk)
        self.nb_plan = nb_plan
        self.nb = int(nb_plan * 1.5) + 5
        camp = parse_campaign(a.campaign)
        storms = []
        for i, (g, s, r) in enumerate(camp):
            storms.append((int(nb_plan * (0.10 + 0.80 * i / len(camp))), g, s, r))
        self.sw = SolarWind(a.seed, self.nb, storms)
        self.true_ts = BASE_TS + self.rng.randint(0, 10 ** 6)
        self.src = 0
        self.step_no = 0
        self.block = -1
        self.G = self.S = self.R = 0
        self.offset = 0.0
        self.rtc_left = 0
        self.rtc_t0 = 0
        self.fade_left = 0
        self.fade_depth = 0.0
        self.blackout_left = 0
        self.bo_ev = None
        self.buf_plain = deque(maxlen=a.buffer_cap)
        self.buf_must = []
        self.delayq = []
        self.restore = []
        self.seq = 0
        self.sos_left = 0
        self.same_left = 0
        self.same_ts = 0
        self.sat_left = 0
        self.reboot_pending = 0
        self.crlf_left = 0
        self.must_ts = set()
        self.must_seen = set()
        self.be_seen = set()
        self.must = []          # (record, class, ts)
        self.besteffort = []    # (record, ts)
        self.truth = []         # [type, first, last, detail]
        self.cov = Counter()
        self.tail = bytearray()
        self.emitted = 0
        self.p_sos = a.sos_rate
        self.gen_rate = a.general_faults / 1e6

    # ---------------- bookkeeping ----------------
    def ev(self, kind, detail=""):
        self.truth.append([kind, self.emitted, self.emitted, detail])
        self.cov["FAULT-" + kind] += 1
        return len(self.truth) - 1

    def next_seq(self):
        self.seq += 1
        return self.seq

    # ---------------- emission ----------------
    def emit_bytes(self, line, rec=None):
        if self.emitted >= self.total:
            raise Done
        term = b"\r\n" if self.crlf_left > 0 else b"\n"
        if self.crlf_left > 0:
            self.crlf_left -= 1
        data = line + term
        self.out.write(data)
        self.tail += data
        if len(self.tail) > 512:
            del self.tail[:-512]
        if rec is not None:
            if rec.must and rec.ts not in self.must_seen:
                self.must_seen.add(rec.ts)
                self.must.append((self.emitted, rec.cls, rec.ts))
                self.cov["PLANTED-" + rec.cls] += 1
            elif rec.victim and rec.ts not in self.be_seen:
                self.be_seen.add(rec.ts)
                self.besteffort.append((self.emitted, rec.ts))
                self.cov["PLANTED-SEU_VICTIM"] += 1
        self.emitted += 1
        if self.emitted % self.a.checkpoint == 0:
            self.out.checkpoint(self)

    def seu(self, line, protect=0):
        r = self.rng
        n = len(line)
        if n <= protect:
            return line
        b = bytearray(line)
        t = r.random()
        p = r.randrange(protect, n)
        if t < 0.80:
            b[p] ^= 1 << r.randrange(8)
            self.cov["SEU-1bit"] += 1
        elif t < 0.95:
            for _ in range(r.randint(2, 4)):
                q = min(n - 1, p + r.randrange(4))
                b[q] ^= 1 << r.randrange(8)
            self.cov["SEU-MBU"] += 1
        else:
            val = r.choice((0x00, 0xFF))
            for q in range(p, min(n, p + r.randint(1, 8))):
                b[q] = val
            self.cov["SEU-stuck"] += 1
        return bytes(b)

    def emit_rec(self, rec):
        line = rec.rest if rec.raw else (b"%d," % rec.ts) + rec.rest
        if not rec.raw:
            if rec.victim:
                line = self.seu(line, protect=line.index(b",") + 1)
            elif not rec.must and self.rng.random() < LV_SEU[self.S]:
                line = self.seu(line)
        self.emit_bytes(line, rec)

    def release_due(self):
        while self.delayq and self.delayq[0][0] <= self.emitted:
            _, _, r = heapq.heappop(self.delayq)
            self.emit_rec(r)

    def out_rec(self, rec):
        self.release_due()
        self.emit_rec(rec)

    def delay(self, rec, k):
        heapq.heappush(self.delayq, (self.emitted + k, self.next_seq(), rec))

    # ---------------- link / transport ----------------
    def atten(self):
        return LV_ATTEN[self.G] + (self.fade_depth if self.fade_left > 0 else 0.0)

    def transport(self, rec):
        r, g = self.rng, self.G
        loss = LV_LOSS[g] + (0.25 if (self.fade_left > 0 and self.fade_depth >= 15) else 0.0)
        if not rec.must and not rec.victim and loss and r.random() < loss:
            self.cov["LOSS"] += 1
            return
        copies = 1
        if LV_DUP[g] and r.random() < LV_DUP[g]:
            copies = 2
            self.cov["DUP"] += 1
        for k in range(copies):
            if k == 1:
                self.delay(rec, r.randint(0, 5))
            elif LV_REORDER[g] and r.random() < LV_REORDER[g]:
                self.delay(rec, r.randint(2, 200))
                self.cov["REORDER"] += 1
            else:
                self.out_rec(rec)

    def process(self, rec):
        if self.blackout_left > 0:
            self.seq += 1
            if rec.must:
                if rec.cls in ("A", "D"):
                    rec.cls = "B"
                self.buf_must.append((self.seq, rec))
            else:
                if len(self.buf_plain) == self.buf_plain.maxlen:
                    self.cov["BLACKOUT-BUFFER-OVERFLOW-DROP"] += 1
                self.buf_plain.append((self.seq, rec))
            self.blackout_left -= 1
            if self.blackout_left == 0:
                self.flush_blackout()
            return
        self.transport(rec)

    def start_blackout(self, n, kind):
        if n <= 0:
            return
        if self.blackout_left > 0:
            self.blackout_left = max(self.blackout_left, n)
            return
        self.blackout_left = n
        self.bo_ev = self.ev(kind, "records=%d" % n)

    def flush_blackout(self):
        items = sorted(list(self.buf_plain) + self.buf_must, key=lambda x: x[0])
        self.buf_plain.clear()
        self.buf_must = []
        recs = [r for _, r in items]
        n = len(recs)
        r = self.rng
        for i in range(n):
            if r.random() < 0.15:
                j = min(n - 1, max(0, i + r.randint(-40, 40)))
                recs[i], recs[j] = recs[j], recs[i]
        out = []
        for x in recs:
            out.append(x)
            if r.random() < 0.05:
                out.append(x)
                self.cov["DUP"] += 1
        if self.bo_ev is not None:
            self.truth[self.bo_ev][3] += " flushed=%d" % len(out)
        try:
            for x in out:
                self.out_rec(x)
        finally:
            if self.bo_ev is not None:
                self.truth[self.bo_ev][2] = self.emitted - 1
                self.bo_ev = None

    # ---------------- clock ----------------
    def obs_ts(self):
        if self.rtc_left > 0:
            ts = RTC_BASE + (self.true_ts - self.rtc_t0)
            return ts, ts - self.true_ts
        sigma = LV_SIGMA[self.G]
        err = int(round(self.offset + (self.rng.gauss(0, sigma) if sigma else 0.0)))
        return self.true_ts + err, err

    # ---------------- record builders ----------------
    def payload(self, bg=False):
        r = self.rng
        if bg:
            x = r.random()
            d = self.a.bg_distress
            st = STATES[3] if x < d else STATES[(0, 1, 2)[r.randrange(3)]] if True else None
        else:
            st = STATES[r.randrange(4)]
        if st == b"DISTRESS":
            return r.uniform(7.5, 8.5), r.uniform(800, 900), r.uniform(-25, -10), st, 1
        if st == b"PRANK":
            return r.uniform(0.3, 0.8), 210.0, r.uniform(-85, -78), st, 0
        return r.uniform(0.8, 1.5), 210.0, r.uniform(-66, -64), st, 0

    def baseline(self):
        r = self.rng
        if self.same_left > 0:
            self.same_left -= 1
        else:
            self.true_ts += r.randint(1, 10)
        c1, c2, c3, st, fl = self.payload(bg=True)
        c3 -= self.atten()
        if self.sat_left > 0:
            self.sat_left -= 1
            c1, c2, c3 = RAILS
            self.cov["SENSOR-SATURATED-RECORDS"] += 1
        ts, _ = self.obs_ts()
        return Rec(ts, b"%.1f,%.1f,%.1f,%s,%d" % (c1, c2, c3, st, fl))

    def sos(self, canary=False):
        r = self.rng
        if self.sos_left == 0:
            self.sos_left = r.randint(1, 8)
        self.sos_left -= 1
        self.true_ts += r.randint(100, 300)
        if canary:
            c1, c2, c3 = 8.1, 850.0, -15.0
        else:
            c1, c2, c3 = r.uniform(7.5, 8.5), r.uniform(800, 900), r.uniform(-25, -10)
        at = self.atten()
        c3 -= at
        ts, err = self.obs_ts()
        victim = (not canary) and r.random() < self.a.victim_p
        if err != 0 or self.rtc_left > 0:
            cls = "C"
        elif at >= 6:
            cls = "D"
        else:
            cls = "A"
        if not victim:
            while ts in self.must_ts:
                ts += 1
            self.must_ts.add(ts)
        else:
            while ts in self.must_ts:
                ts += 1
            self.must_ts.add(ts)
        return Rec(ts, b"%.1f,%.1f,%.1f,DISTRESS,1" % (c1, c2, c3), must=not victim, cls=cls, victim=victim)

    # ---------------- environment events ----------------
    def on_block(self, b):
        r = self.rng
        pG, pS, pR = self.G, self.S, self.R
        self.block = b
        self.G, self.S, self.R = self.sw.G[b], self.sw.S[b], self.sw.R[b]
        if self.G >= 1 and pG == 0:
            step = r.uniform(20, 200) * self.G
            self.offset += step
            self.ev("SSC_CLOCK_STEP", "G%d +%.0f ms" % (self.G, step))
        if self.G == 0 and pG > 0 and self.offset != 0.0:
            self.ev("GNSS_REACQUIRE_STEP", "%.0f ms" % -self.offset)
            self.offset = 0.0
        if self.R > pR:
            self.start_blackout(LV_R_BLACK[self.R], "R%d_FLARE_BLACKOUT" % self.R)
        elif self.G >= 3 and r.random() < LV_GBLACK_P[self.G]:
            self.start_blackout(LV_GBLACK_LEN[self.G], "G%d_IONOSPHERIC_BLACKOUT" % self.G)
        lam = (LV_BROWN[self.G] + LV_SEL[self.S]) * self.blk / 10000.0
        self.reboot_pending += int(lam) + (1 if r.random() < lam - int(lam) else 0)

    def reboot(self):
        r = self.rng
        self.reboot_pending -= 1
        e = self.ev("REBOOT_RTC_RESET", "brownout/latch-up")
        c1, c2, c3, st, fl = self.payload()
        line = b"%d,%.1f,%.1f,%.1f,%s,%d" % (self.true_ts, c1, c2, c3, st, fl)
        self.process(Rec(0, line[: r.randint(1, len(line) - 1)], raw=True))            # power lost mid-write
        self.process(Rec(0, b"\x00" * r.choice((512, 4096)), raw=True))                # zero-filled sector
        self.rtc_left = r.randint(2000, 20000)
        self.rtc_t0 = self.true_ts
        for i in range(r.randint(2, 6)):
            self.process(Rec(RTC_BASE + i, b"0.0,0.0,0.0,BOOT,0"))
        self.truth[e][2] = self.emitted

    def general_fault(self):
        r = self.rng
        kind = r.choice(["leap_second", "ntp_step", "epoch_edge", "gps_week_rollover", "zero_sector", "dup_sector",
                         "truncated_write", "idle_gap", "same_ms_burst", "eol_switch"])
        if kind == "leap_second":
            self.offset -= 1000.0
            heapq.heappush(self.restore, (self.step_no + 20, self.next_seq(), 1000.0))
            self.ev("LEAP_SECOND", "clock steps back 1 s for 20 records")
        elif kind == "ntp_step":
            d = r.choice((-1, 1)) * r.randint(1000, 3600000)
            self.offset += d
            heapq.heappush(self.restore, (self.step_no + r.randint(500, 5000), self.next_seq(), -float(d)))
            self.ev("NTP_STEP", "%+d ms" % d)
        elif kind == "gps_week_rollover":
            self.offset -= GPS_WEEK_MS
            heapq.heappush(self.restore, (self.step_no + r.randint(20, 300), self.next_seq(), float(GPS_WEEK_MS)))
            self.ev("GPS_WEEK_ROLLOVER", "-1024 weeks")
        elif kind == "epoch_edge":
            e = self.ev("EPOCH_EDGE_VALUES", "%d records" % len(EDGE_TS))
            for t in EDGE_TS:
                c1, c2, c3, st, fl = self.payload()
                self.process(Rec(t, b"%.1f,%.1f,%.1f,%s,%d" % (c1, c2, c3, st, fl)))
            self.truth[e][2] = self.emitted
        elif kind == "zero_sector":
            self.ev("ZERO_FILLED_SECTOR")
            self.process(Rec(0, b"\x00" * r.choice((512, 4096)), raw=True))
        elif kind == "dup_sector":
            self.ev("DUPLICATED_SECTOR")
            self.process(Rec(0, bytes(self.tail) or b"x", raw=True))
        elif kind == "truncated_write":
            self.ev("TRUNCATED_WRITE")
            c1, c2, c3, st, fl = self.payload()
            line = b"%d,%.1f,%.1f,%.1f,%s,%d" % (self.true_ts, c1, c2, c3, st, fl)
            self.process(Rec(0, line[: r.randint(1, len(line) - 1)], raw=True))
        elif kind == "idle_gap":
            g = r.randint(60000, 7200000)
            self.true_ts += g
            self.ev("IDLE_GAP", "%d ms" % g)
        elif kind == "same_ms_burst":
            self.same_left = r.randint(50, 500)
            self.ev("SAME_MS_BURST", "%d records" % self.same_left)
        else:
            self.crlf_left = r.randint(200, 2000)
            self.ev("EOL_SWITCH_CRLF", "%d records" % self.crlf_left)

    # ---------------- main step ----------------
    def step(self):
        r = self.rng
        self.src += 1
        self.step_no += 1
        b = min(self.src // self.blk, self.nb - 1)
        if b != self.block:
            self.on_block(b)
        while self.restore and self.restore[0][0] <= self.step_no:
            _, _, d = heapq.heappop(self.restore)
            self.offset += d
        if self.G >= 3:
            self.offset += LV_HOLD[self.G]
        if self.fade_left > 0:
            self.fade_left -= 1
        elif self.G > 0 and r.random() < LV_FADE_RATE[self.G] / 10000.0:
            self.fade_left = r.randint(5, 60)
            self.fade_depth = LV_FADE_DEPTH[self.G] * r.uniform(0.5, 1.0)
            self.cov["FADE-BURST"] += 1
        if self.rtc_left > 0:
            self.rtc_left -= 1
            if self.rtc_left == 0:
                self.ev("RTC_RESYNC", "clock back to GNSS")
        if self.reboot_pending and r.random() < 0.002:
            self.reboot()
        if self.sat_left == 0 and self.S >= 2 and r.random() < 0.0004 * self.S:
            self.sat_left = r.randint(20, 200)
            self.ev("SENSOR_SATURATION", "%d records" % self.sat_left)
        if self.gen_rate and r.random() < self.gen_rate:
            self.general_fault()
        if self.a.canary_every and self.src % self.a.canary_every == 0:
            rec = self.sos(canary=True)
        elif self.sos_left > 0 or r.random() < self.p_sos * (1 + self.G):
            rec = self.sos()
        else:
            rec = self.baseline()
        self.process(rec)

    def run(self):
        try:
            if self.a.partial_start:
                c1, c2, c3, st, fl = self.payload()
                line = b"%d,%.1f,%.1f,%.1f,%s,%d" % (self.true_ts, c1, c2, c3, st, fl)
                self.ev("ATTACH_MID_LINE", "stream starts inside a record")
                self.emit_bytes(line[self.rng.randint(5, len(line) - 3):])
            while True:
                self.step()
        except Done:
            pass
        self.out.checkpoint(self)


# =============================================================================================
# sinks
# =============================================================================================
def emit_fragments(fp, data, frag_rng):
    if frag_rng is None:
        fp.write(data)
        return
    i, n = 0, len(data)
    while i < n:
        m = frag_rng.choice(FRAG_SIZES)
        fp.write(data[i:i + m])
        fp.flush()
        i += m


class FileOut:
    def __init__(self, fp, a):
        self.fp, self.a = fp, a
        self.buf = bytearray()
        self.frag = random.Random(a.seed ^ 0xF4A6) if a.fragment else None
        self.t0 = time.time()
        self.sha = hashlib.sha256()
        self.bytes = 0

    def write(self, b):
        self.buf += b

    def checkpoint(self, sim):
        if not self.buf:
            return
        data = bytes(self.buf)
        self.buf.clear()
        self.sha.update(data)
        self.bytes += len(data)
        try:
            emit_fragments(self.fp, data, self.frag)
        except BrokenPipeError:
            raise Done
        if self.a.rate > 0:
            lag = sim.emitted / self.a.rate - (time.time() - self.t0)
            if lag > 0:
                time.sleep(lag)


class ProcOut:
    def __init__(self, proc, h, a):
        self.proc, self.h, self.a = proc, h, a
        self.buf = bytearray()
        self.seen = 0
        self.seen_be = 0
        self.frag = random.Random(a.seed ^ 0xF4A6) if a.fragment else None
        self.t0 = time.time()

    def write(self, b):
        self.buf += b

    def checkpoint(self, sim):
        for _rec, cls, ts in sim.must[self.seen:]:        # register BEFORE the engine can see the bytes
            self.h["must"][ts] = cls
        self.seen = len(sim.must)
        for _rec, ts in sim.besteffort[self.seen_be:]:
            self.h["be"].add(ts)
        self.seen_be = len(sim.besteffort)
        if not self.buf:
            return
        data = bytes(self.buf)
        self.buf.clear()
        try:
            emit_fragments(self.proc.stdin, data, self.frag)
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError, ValueError):
            self.h["crashed_at"] = sim.emitted
            raise Done
        self.h["progress"] = time.time()
        if self.a.rate > 0:
            lag = sim.emitted / self.a.rate - (time.time() - self.t0)
            if lag > 0:
                time.sleep(lag)
            self.h["progress"] = time.time()


# =============================================================================================
# harness
# =============================================================================================
def run_harness(a, cmd, quiet=False):
    import subprocess
    use_stdout = a.engine_out == "stdout"
    try:
        proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE if use_stdout else subprocess.DEVNULL)
    except OSError as e:
        print("[HARNESS] cannot start engine %r: %s" % (cmd, e))
        return 3
    h = {"must": {}, "be": set(), "progress": time.time(), "stalled": False, "done": False, "crashed_at": None}
    found, found_be = set(), set()

    def scan(tok):
        if tok.isdigit():
            v = int(tok)
            if v in h["must"]:
                found.add(v)
            if v in h["be"]:
                found_be.add(v)

    def reader():
        for line in proc.stdout:
            scan(line.split(b",", 1)[0].strip())

    def watchdog():
        while not h["done"]:
            time.sleep(0.25)
            if not h["done"] and time.time() - h["progress"] > a.stall_timeout:
                h["stalled"] = True
                try:
                    proc.kill()
                except OSError:
                    pass
                return

    rd = threading.Thread(target=reader, daemon=True) if use_stdout else None
    if rd:
        rd.start()
    threading.Thread(target=watchdog, daemon=True).start()
    out = ProcOut(proc, h, a)
    sim = Sim(a, out)
    t0 = time.time()
    sim.run()
    h["done"] = True
    try:
        proc.stdin.close()
    except (BrokenPipeError, OSError, ValueError):
        pass
    import subprocess as sp
    hung = False
    try:
        rc = proc.wait(timeout=a.stall_timeout)
    except sp.TimeoutExpired:
        proc.kill()
        rc = proc.wait()
        hung = True
    if rd:
        rd.join(timeout=10)
    elif os.path.exists(a.engine_out):
        with open(a.engine_out, "rb") as f:
            for line in f:
                scan(line.split(b",", 1)[0].strip())
    elapsed = time.time() - t0
    crashed = (h["crashed_at"] is not None and not h["stalled"]) or (rc is not None and rc < 0 and not h["stalled"] and not hung)
    engine_fail = crashed or h["stalled"] or hung
    classes = {}
    for ts, cls in h["must"].items():
        c = classes.setdefault(cls, {"total": 0, "forwarded": 0})
        c["total"] += 1
        c["forwarded"] += ts in found
    for c in classes.values():
        c["missed"] = c["total"] - c["forwarded"]
        c["recall"] = round(c["forwarded"] / c["total"], 6) if c["total"] else None
    req = [x.strip().upper() for x in a.require.split(",") if x.strip()]
    req_missed = sum(classes.get(k, {}).get("missed", 0) for k in req)
    res = {"version": VERSION, "seed": a.seed, "campaign": a.campaign, "records_sent": sim.emitted,
           "seconds": round(elapsed, 2), "records_per_s": round(sim.emitted / max(elapsed, 1e-9)),
           "engine_exit_code": rc, "engine_crashed": crashed, "engine_stalled": h["stalled"], "engine_hung_at_exit": hung,
           "classes": dict(sorted(classes.items())), "required_classes": req, "required_missed": req_missed,
           "seu_victims_total": len(h["be"]), "seu_victims_forwarded_best_effort": len(found_be),
           "result": "FAIL" if (engine_fail or req_missed) else "PASS"}
    with open(a.result, "w") as f:
        json.dump(res, f, indent=1)
    write_side_files(a, sim)
    if not quiet:
        print("records sent      : %s in %.1fs (%s rec/s)" % (format(sim.emitted, ","), elapsed, format(res["records_per_s"], ",")))
        print("engine exit code  : %s%s%s%s" % (rc, "  CRASHED" if crashed else "", "  STALLED" if h["stalled"] else "", "  HUNG-AT-EXIT" if hung else ""))
        for k, c in sorted(classes.items()):
            print("rescue class %s    : %6d planted | forwarded %6d | MISSED %d%s" % (k, c["total"], c["forwarded"], c["missed"], "   <- required" if k in req else ""))
        print("SEU victims       : %d planted | %d forwarded (best effort, not scored)" % (len(h["be"]), len(found_be)))
        print("RESULT: %s   (details: %s)" % (res["result"], a.result))
    if engine_fail:
        return 3
    return 1 if req_missed else 0


# =============================================================================================
# reference engines for --selftest
# =============================================================================================
def ref_engine(mode):
    rx = re.compile(rb"^(\d+),-?\d+\.\d+,-?\d+\.\d+,-?\d+\.\d+,DISTRESS,1$")
    out, n, mx = sys.stdout.buffer, 0, 0
    for line in sys.stdin.buffer:
        n += 1
        if mode == "crash" and n >= 20000:
            os._exit(7)
        if mode == "hang" and n >= 20000:
            time.sleep(3600)
        m = rx.match(line.rstrip(b"\r\n"))
        if not m:
            continue
        ts = int(m.group(1))
        if mode == "leaky" and ts % 3 == 0:
            continue
        if mode == "strictclock":
            late = ts < mx - 1000
            mx = max(mx, ts)
            if late or ts < 1_000_000_000_000 or ts > 2_000_000_000_000:
                continue
        out.write(m.group(1) + b"\n")
        if n % 1000 == 0:
            out.flush()
    out.flush()


def selftest(a):
    import tempfile
    me = [sys.executable, os.path.abspath(__file__), "--ref-engine"]
    ok = True
    cases = [("good", "A", 0), ("good", "A,B,C,D", 0), ("leaky", "A", 1), ("strictclock", "A", 0),
             ("strictclock", "A,B,C,D", 1), ("crash", "A", 3), ("hang", "A", 3)]
    for mode, req, want in cases:
        b = argparse.Namespace(**vars(a))
        b.total, b.seed, b.campaign, b.sos_rate, b.canary_every = 300000, 3, "G3,G4,G5", 0.002, 20000
        b.stall_timeout, b.require, b.rate, b.fragment, b.engine_out = 6.0, req, 0, False, "stdout"
        b.result = os.path.join(tempfile.gettempdir(), "spacewx_selftest_%s.json" % mode)
        b.timeline = b.truth = b.must = b.coverage = b.file = None
        got = run_harness(b, me + [mode], quiet=True)
        extra = ""
        if mode == "strictclock" and req == "A":
            cl = json.load(open(b.result))["classes"]
            lower = [k for k in "BC" if cl.get(k, {}).get("recall") is not None and cl[k]["recall"] < 1.0]
            extra = " (class recall < 100%% for %s as intended)" % ",".join(lower) if lower else " (B/C recall unexpectedly 100%)"
            good = got == want and bool(lower)
        else:
            good = got == want
        ok = ok and good
        print("[selftest] engine=%-11s require=%-8s expected exit %d, got %d  %s%s" % (mode, req, want, got, "OK" if good else "MISMATCH", extra))
    print("SELFTEST: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


# =============================================================================================
# main
# =============================================================================================
def write_side_files(a, sim):
    if a.timeline:
        sim.sw.write_timeline(a.timeline, sim.blk, sim.nb_plan)
    if a.truth:
        with open(a.truth, "w") as f:
            f.write("fault_type,first_record,last_record,detail\n")
            for k, x, y, d in sim.truth:
                f.write("%s,%d,%d,%s\n" % (k, x, y, d.replace(",", ";")))
    if a.must:
        with open(a.must, "w") as f:
            f.write("record,class,ts\n")
            for rec, cls, ts in sim.must:
                f.write("%d,%s,%d\n" % (rec, cls, ts))
    if a.coverage:
        with open(a.coverage, "w") as f:
            json.dump(dict(sorted(sim.cov.items())), f, indent=1)


def main():
    p = argparse.ArgumentParser(description="Shuzi SPACEWX v%s: space-weather fault generator + rescue-path harness" % VERSION)
    p.add_argument("--version", action="version", version="shuzi_spacewx v%s" % VERSION)
    p.add_argument("--total", type=int, default=1_000_000, help="records to emit")
    p.add_argument("--seed", type=int, default=20261115)
    p.add_argument("--campaign", default="campaign", help="campaign | quiet | moderate | severe | extreme | G3,G1S2R1,...")
    p.add_argument("--block", type=int, default=0, help="records per solar-wind block (default total/1000, min 20)")
    p.add_argument("--sos-rate", type=float, default=3e-4, help="rescue burst start probability per source record (x(1+G) in storms)")
    p.add_argument("--victim-p", type=float, default=0.03, help="share of rescue lines that get an SEU bit flip (best effort)")
    p.add_argument("--bg-distress", type=float, default=0.05, help="share of ordinary traffic that is DISTRESS state")
    p.add_argument("--canary-every", type=int, default=50000, help="plant a standard rescue signal every N source records (0 off)")
    p.add_argument("--general-faults", type=float, default=20.0, help="time/storage faults per million records (0 off)")
    p.add_argument("--buffer-cap", type=int, default=4096, help="blackout buffer capacity in non-rescue records")
    p.add_argument("--partial-start", action="store_true", help="start the stream in the middle of a record")
    p.add_argument("--checkpoint", type=int, default=2000, help="records per write/registration checkpoint")
    p.add_argument("--run", default=None, metavar="CMD", help="spawn your engine and verify rescue-path delivery")
    p.add_argument("--engine-out", default="stdout", help="'stdout' or a file the engine writes forwarded timestamps to")
    p.add_argument("--stall-timeout", type=float, default=120.0)
    p.add_argument("--require", default="A", help="rescue classes that must be perfect, e.g. A or A,B,C,D")
    p.add_argument("--result", default="spacewx_result.json")
    p.add_argument("--stdout", action="store_true", help="stream to stdout")
    p.add_argument("--file", default=None, help="write the stream to FILE and print its SHA-256")
    p.add_argument("--fragment", action="store_true", help="split the stream at random byte boundaries")
    p.add_argument("--rate", type=float, default=0, help="throttle to ~N records/s")
    p.add_argument("--timeline", default=None)
    p.add_argument("--truth", default=None)
    p.add_argument("--must", default=None)
    p.add_argument("--coverage", default=None)
    p.add_argument("--selftest", action="store_true")
    p.add_argument("--ref-engine", default=None, choices=["good", "leaky", "strictclock", "crash", "hang"], help=argparse.SUPPRESS)
    a = p.parse_args()
    if a.ref_engine:
        return ref_engine(a.ref_engine)
    if a.selftest:
        sys.exit(selftest(a))
    parse_campaign(a.campaign)
    if a.run:
        import shlex
        sys.exit(run_harness(a, shlex.split(a.run, posix=(os.name != "nt"))))
    if a.file:
        with open(a.file, "wb") as fp:
            out = FileOut(fp, a)
            sim = Sim(a, out)
            sim.run()
        print("%s  %s  (%s records, %s bytes)" % (out.sha.hexdigest(), a.file, format(sim.emitted, ","), format(out.bytes, ",")), file=sys.stderr)
    else:
        out = FileOut(sys.stdout.buffer, a)
        sim = Sim(a, out)
        sim.run()
    write_side_files(a, sim)


if __name__ == "__main__":
    main()
