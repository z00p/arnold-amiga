"""Frame-budget runs per baseline machine (WinUAE), for the readme's "What to expect".

Runs the titles in tests/perf_titles.csv (Burnin' Rubber, Puzzle Bobble, Ghosts'n
Goblins GX, Sonic GX; sound on, real 50 Hz pacing) through tools/regress.py --perf
with a given config and binary. Take the WinUAE lock first.

    python -I tools/perf_machines.py --config Arnold-030.uae --exe Arnold-030 --run perf07-030
    python -I tools/perf_machines.py --config Arnold-060-CE.uae --exe Arnold-060 --run perf07-060

WinUAE's 68030 and 68060 timing is approximate: treat the figures as estimates.
"""
import csv, os, sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
import regress  # noqa: E402

CSV = os.path.join(os.path.dirname(TOOLS), "tests", "perf_titles.csv")
regress.titles = lambda: list(csv.DictReader(open(CSV, encoding="utf-8")))
sys.argv = [sys.argv[0], "full", "--perf", "--sound", "--timeout", "1500"] + sys.argv[1:]
regress.main()
