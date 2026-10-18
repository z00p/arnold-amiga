"""Tiered regression runner for Arnold on Amiga (after AmiMSX's tools/regress.py).

Tiers:  smoke              titles with smoke=Y in tests/titles.csv
        full               every title
        ids:<id,id,...>    named titles

All titles run in one WinUAE boot (winuae/Arnold-PiStorm.uae: 68040 JIT; an
AmigaDOS script runs Arnold once per title, TURBO = no 50 Hz wait) with its key
script (tests/keys/<keys>.keys) plus SNAP markers at its shot times and a QUIT
after the last one. Arnold itself writes every SNAP as an exact CPC frame
(run/snaps/<id>/<frame>.png) and a "done" file at QUIT, so no screen grabs are
needed. Captures are compared pixel for pixel with the accepted baseline
(build/regress/baseline/<id>/); the report lists new, changed and missing
captures with a side-by-side sheet of the changed ones.

Usage:
    python -I tools/regress.py smoke [--run NAME] [--timeout 180] [--single] [--no-turbo]
    python -I tools/regress.py full --accept        (run, then make it the baseline)
    python -I tools/regress.py --accept-run NAME    (accept an earlier run)
"""
import argparse, csv, datetime, os, re, shutil, subprocess, sys, time
from PIL import Image, ImageChops

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUN = os.path.join(BASE, "run")
REG = os.path.join(BASE, "build", "regress")
BASELINE = os.path.join(REG, "baseline")
SEQ = os.path.join(BASE, "winuae", "wb", "S", "Startup-Sequence")
UAE = os.path.join(BASE, "winuae", "bin", "arnold-uae64.exe")


def titles():
    with open(os.path.join(BASE, "tests", "titles.csv"), encoding="utf-8") as f:
        return list(csv.DictReader(f))


def select(rows, tier):
    if tier == "smoke":
        return [r for r in rows if r["smoke"] == "Y"]
    if tier == "full":
        return rows
    if tier.startswith("ids:"):
        ids = tier[4:].split(",")
        return [r for r in rows if r["id"] in ids]
    raise SystemExit(f"unknown tier {tier}")


def frames(t):
    t = t.strip()
    return int(round(float(t[:-1]) * 50)) if t.endswith("s") else int(t)


def write_keys(r):
    """tests/keys/<keys>.keys + SNAP lines + QUIT -> run/Keys/<id>.keys"""
    src = os.path.join(BASE, "tests", "keys", r["keys"] + ".keys")
    body = open(src, encoding="utf-8").read() if os.path.exists(src) else ""
    shots = [frames(s) for s in r["shots"].split(",") if s.strip()]
    lines = [body.rstrip(), ""] + [f"{s} SNAP" for s in shots] + [f"{max(shots) + 25} QUIT", ""]
    os.makedirs(os.path.join(RUN, "Keys"), exist_ok=True)
    with open(os.path.join(RUN, "Keys", r["id"] + ".keys"), "w", encoding="latin-1", newline="\n") as f:
        f.write("\n".join(lines))
    return shots


STARTLINE = r"(?m)^Run >NIL: (Arnold:Arnold|Arnold:AmiCPC|Execute Arnold:Keys/batch).*$"
EXE = "Arnold"                       # --exe: Arnold (68040), Arnold-060, ... (binaries in run/)


def set_startup(line):
    s = open(SEQ, encoding="latin-1").read()
    s = re.sub(STARTLINE, line.rstrip(), s)
    open(SEQ, "w", encoding="latin-1", newline="\n").write(s)


def kill_uae():
    subprocess.run(["taskkill", "/F", "/IM", "arnold-uae64.exe"], capture_output=True)


SOUND = False            # --sound drops NOSOUND
EXTRA_ARGS = ""          # --args adds Arnold arguments


def arnold_args(r, turbo):
    media = f"Arnold:{r['file']} " if r["file"] else ""
    return (f"{media}KEYS=Arnold:Keys/{r['id']}.keys SNAPDIR=Arnold:snaps/{r['id']}"
            + ("" if SOUND else " NOSOUND") + (" TURBO" if turbo else "") + EXTRA_ARGS)


def prepare(r):
    snapdir = os.path.join(RUN, "snaps", r["id"])
    shutil.rmtree(snapdir, ignore_errors=True)
    os.makedirs(snapdir)
    write_keys(r)
    return os.path.join(snapdir, "done")


def run_batch(rows, config, timeout, turbo):
    """One WinUAE boot: an AmigaDOS script runs Arnold once per title, in turn.
    Returns {id: (ok, seconds that title took)}; the first includes the boot."""
    dones = [prepare(r) for r in rows]
    with open(os.path.join(RUN, "Keys", "batch"), "w", encoding="latin-1", newline="\n") as f:
        f.write("; written by tools/regress.py\n")
        for r in rows:
            f.write(f"Arnold:{EXE} " + arnold_args(r, turbo) + "\n")
    set_startup("Run >NIL: Execute Arnold:Keys/batch")
    kill_uae()
    t0 = time.time()
    p = subprocess.Popen([UAE, "-f", os.path.join(BASE, "winuae", config)], cwd=os.path.dirname(UAE))
    res, start = {}, t0
    for r, done in zip(rows, dones):
        while time.time() - start < timeout and not os.path.exists(done):
            time.sleep(0.5)
        ok = os.path.exists(done)
        res[r["id"]] = (ok, round(time.time() - start, 1))
        print(f"  {r['id']}: {'done' if ok else 'TIMEOUT'} after {res[r['id']][1]}s", flush=True)
        if not ok:
            break                       # the batch is stuck on this title: the rest won't run
        start = time.time()
    time.sleep(1)
    kill_uae()
    p.wait(timeout=10)
    print(f"  batch total {round(time.time() - t0, 1)}s")
    for r in rows:
        res.setdefault(r["id"], (False, 0.0))
    return res


def run_title(r, config, timeout, turbo):
    done = prepare(r)
    set_startup(f"Run >NIL: Arnold:{EXE} " + arnold_args(r, turbo))
    kill_uae()
    t0 = time.time()
    p = subprocess.Popen([UAE, "-f", os.path.join(BASE, "winuae", config)], cwd=os.path.dirname(UAE))
    while time.time() - t0 < timeout and not os.path.exists(done):
        time.sleep(1)
    ok = os.path.exists(done)
    time.sleep(1)
    kill_uae()
    p.wait(timeout=10)
    return ok, round(time.time() - t0, 1)


def is_capture(f):
    """A frame dump (00400.png), not a comparison sheet or the done file."""
    return f.endswith(".png") and f[:-4].isdigit()


def accept_dir(src, tid):
    dst = os.path.join(BASELINE, tid)
    shutil.rmtree(dst, ignore_errors=True)
    os.makedirs(dst)
    for f in os.listdir(src):
        if is_capture(f):
            shutil.copy2(os.path.join(src, f), dst)


def read_perf(done):
    """'perf key=value ...' lines of Arnold's done file -> {key: int}"""
    pf = {}
    if os.path.exists(done):
        for line in open(done, encoding="latin-1"):
            if line.startswith("perf "):
                for kv in line.split()[1:]:
                    k, _, v = kv.partition("=")
                    if v.isdigit():
                        pf[k] = int(v)
    return pf


def compare(a, b):
    ia, ib = Image.open(a).convert("RGB"), Image.open(b).convert("RGB")
    if ia.size != ib.size:
        return False, "size"
    diff = ImageChops.difference(ia, ib).getbbox()
    return diff is None, diff


def sheet(path, base, new):
    ia, ib = Image.open(base).convert("RGB"), Image.open(new).convert("RGB")
    w, h = max(ia.width, ib.width), max(ia.height, ib.height)
    out = Image.new("RGB", (w * 2 + 8, h), (255, 255, 255))
    out.paste(ia, (0, 0)); out.paste(ib, (w + 8, 0))
    out.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tier", nargs="?", default="smoke")
    ap.add_argument("--run", default=datetime.datetime.now().strftime("%Y%m%d-%H%M"))
    ap.add_argument("--config", default="Arnold-PiStorm.uae")
    ap.add_argument("--no-turbo", action="store_true", help="real 50 Hz pacing, screen every frame")
    ap.add_argument("--single", action="store_true", help="one WinUAE boot per title (old way)")
    ap.add_argument("--exe", default="Arnold", help="binary in run/: Arnold (68040), Arnold-040-Max, Arnold-060, Arnold-060-Max")
    ap.add_argument("--perf", action="store_true",
                    help="real 50 Hz pacing; report each title's frame budget (Arnold's E-clock figures)")
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--sound", action="store_true", help="run with Paula sound on")
    ap.add_argument("--args", default="", help="extra Arnold arguments, e.g. SKIP=0")
    ap.add_argument("--accept", action="store_true")
    ap.add_argument("--accept-run")
    a = ap.parse_args()
    global SOUND, EXTRA_ARGS
    SOUND = a.sound
    EXTRA_ARGS = (" " + a.args) if a.args else ""

    if a.accept_run:
        src = os.path.join(REG, a.accept_run)
        for tid in os.listdir(src):
            if os.path.isdir(os.path.join(src, tid)) and tid != "report.md":
                accept_dir(os.path.join(src, tid), tid)
        print("baseline <-", a.accept_run)
        return

    rows = select(titles(), a.tier)
    outdir = os.path.join(REG, a.run)
    os.makedirs(outdir, exist_ok=True)
    report = [f"# Arnold regression {a.run}: tier {a.tier}, {len(rows)} titles, config {a.config}", ""]
    changed = new = missing = same = 0
    global EXE
    EXE = a.exe
    turbo = not (a.no_turbo or a.perf)
    perf_rows = []
    t_all = time.time()
    batch = None if a.single else run_batch(rows, a.config, a.timeout, turbo)
    for r in rows:
        ok, secs = batch[r["id"]] if batch else run_title(r, a.config, a.timeout, turbo)
        dst = os.path.join(outdir, r["id"])
        shutil.rmtree(dst, ignore_errors=True)
        shutil.copytree(os.path.join(RUN, "snaps", r["id"]), dst)
        pngs = sorted(f for f in os.listdir(dst) if is_capture(f))
        line = f"- **{r['id']}** ({secs}s{'' if ok else ', TIMEOUT'}): "
        parts = []
        for f in pngs:
            b = os.path.join(BASELINE, r["id"], f)
            if not os.path.exists(b):
                parts.append(f"{f} new"); new += 1
            else:
                eq, where = compare(b, os.path.join(dst, f))
                if eq:
                    same += 1
                else:
                    parts.append(f"{f} CHANGED {where}"); changed += 1
                    sheet(os.path.join(dst, f[:-4] + "-vs-baseline.png"), b, os.path.join(dst, f))
        for f in (sorted(os.listdir(os.path.join(BASELINE, r["id"]))) if os.path.isdir(os.path.join(BASELINE, r["id"])) else []):
            if is_capture(f) and f not in pngs:
                parts.append(f"{f} MISSING"); missing += 1
        report.append(line + (", ".join(parts) if parts else f"{len(pngs)} captures match"))
        if a.perf:
            perf_rows.append((r["id"], read_perf(os.path.join(dst, "done"))))
        print(report[-1])
    if a.perf:
        report += ["", f"## Frame budget ({EXE}, {a.config}; times in emulated-machine time)", "",
                   "| Title | fps | core ms/frame | c2p ms/shown frame | chip KB/shown frame | budget used |",
                   "|---|---|---|---|---|---|"]
        for tid, pf in perf_rows:
            if not pf:
                report.append(f"| {tid} | no figures | | | | |"); continue
            report.append(f"| {tid} | {pf['fps_x10'] / 10:.1f} | {pf['core_us_per_frame'] / 1000:.1f} | "
                          f"{pf['c2p_us_per_shown_frame'] / 1000:.1f} | {pf['chip_bytes_per_shown_frame'] / 1024:.0f} | "
                          f"{pf['budget_percent']}% |")
    report += ["", f"Same {same}, changed {changed}, new {new}, missing {missing}. "
               f"{'Batch' if batch else 'Single'} run, turbo {'on' if turbo else 'off'}, {round(time.time() - t_all)}s in all."]
    open(os.path.join(outdir, "report.md"), "w", encoding="utf-8").write("\n".join(report) + "\n")
    print("\n".join(report[-2:]))
    if a.accept:
        a.accept_run = a.run
        for tid in [r["id"] for r in rows]:
            accept_dir(os.path.join(outdir, tid), tid)
        print("accepted as baseline")


if __name__ == "__main__":
    main()
