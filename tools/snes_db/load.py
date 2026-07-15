#!/usr/bin/env python3
"""Persist every SNES analysis result into one SQLite DB — nothing lives in /tmp.

Tables:
  sound_survey  — per-ROM sound-driver scan (tools/snes_survey TSV)
  spin_sweep    — per-ROM spin-loop analysis (tools/snes_spin TSV)
  measurement   — M7 rig / host measurements (the fps ladder), hand-curated rows

Idempotent: each import is keyed by (source_set, filename) and REPLACEs, so
re-loading a TSV updates rather than duplicates. Run:
  python3 tools/snes_db/load.py sound  <tsv> <source_set>
  python3 tools/snes_db/load.py spin   <tsv> <source_set>
  python3 tools/snes_db/load.py gate   <tsv> <source_set>   (spin-skip A/B policy)
  python3 tools/snes_db/load.py measure <tsv-with-header>   (game\tconfig\temu\tapu\ttotal\tfps\tstatehash\tnote)
  python3 tools/snes_db/load.py report
DB: tools/snes_db/snes_analysis.sqlite (gitignored — ROM filename lists stay off the public repo).
"""
import sqlite3
import sys
import os
import datetime

DB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "snes_analysis.sqlite")

SCHEMA = """
CREATE TABLE IF NOT EXISTS sound_survey(
  source_set TEXT NOT NULL, filename TEXT NOT NULL,
  status TEXT, lit INTEGER, sigs TEXT, nspc_params TEXT,
  loaded_at TEXT, PRIMARY KEY(source_set, filename));
CREATE TABLE IF NOT EXISTS spin_sweep(
  source_set TEXT NOT NULL, filename TEXT NOT NULL,
  status TEXT, lit INTEGER, pure_spin REAL, io_spin REAL, wai INTEGER,
  top_site TEXT, top_addr TEXT,
  loaded_at TEXT, PRIMARY KEY(source_set, filename));
CREATE TABLE IF NOT EXISTS measurement(
  game TEXT NOT NULL, config TEXT NOT NULL,
  emu_insn INTEGER, apu_insn INTEGER, total_insn INTEGER, fps REAL,
  statehash TEXT, note TEXT, measured_at TEXT,
  PRIMARY KEY(game, config));
CREATE TABLE IF NOT EXISTS spin_gate(
  source_set TEXT NOT NULL, filename TEXT NOT NULL,
  status TEXT, lit INTEGER, hash_stock TEXT, hash_skip TEXT,
  hash_match TEXT, ops_skipped REAL, ms_stock REAL, ms_skip REAL,
  verdict TEXT,
  loaded_at TEXT, PRIMARY KEY(source_set, filename));
"""


def now():
    return datetime.datetime.now().isoformat(timespec="seconds")


def load_sound(db, tsv, source_set):
    n = 0
    for line in open(tsv, encoding="utf-8", errors="replace"):
        p = line.rstrip("\n").split("\t")
        if len(p) < 4:
            continue
        row = (source_set, p[0], p[1],
               int(p[2]) if p[2].isdigit() else None,
               p[3], p[4] if len(p) > 4 else None, now())
        db.execute("REPLACE INTO sound_survey VALUES(?,?,?,?,?,?,?)", row)
        n += 1
    return n


def load_spin(db, tsv, source_set):
    n = 0
    for line in open(tsv, encoding="utf-8", errors="replace"):
        p = line.rstrip("\n").split("\t")
        if len(p) < 8:
            continue
        def num(x):
            try:
                return float(x)
            except ValueError:
                return None
        row = (source_set, p[0], p[1],
               int(p[2]) if p[2].isdigit() else None,
               num(p[3]), num(p[4]),
               int(p[5]) if p[5].isdigit() else None,
               p[6], p[7], now())
        db.execute("REPLACE INTO spin_sweep VALUES(?,?,?,?,?,?,?,?,?,?)", row)
        n += 1
    return n


def load_gate(db, tsv, source_set):
    """A/B gate TSV: filename status lit hash_stock hash_skip MATCH/DIVERGE skip% ms0 ms1.
    Policy verdict derived here: SKIP_ON (MATCH >=20%), SKIP_OFF (MATCH <20%),
    EXCLUDE (DIVERGE), BROKEN (either side failed)."""
    n = 0
    for line in open(tsv, encoding="utf-8", errors="replace"):
        p = line.rstrip("\n").split("\t")
        if len(p) < 9:
            continue
        def num(x):
            try:
                return float(x)
            except ValueError:
                return None
        pct = num(p[6])
        if p[5] == "MATCH":
            verdict = "SKIP_ON" if (pct or 0) >= 20.0 else "SKIP_OFF"
        elif p[5] == "DIVERGE":
            verdict = "EXCLUDE"
        else:
            verdict = "BROKEN"
        row = (source_set, p[0], p[1],
               int(p[2]) if p[2].isdigit() else None,
               p[3], p[4], p[5], pct, num(p[7]), num(p[8]), verdict, now())
        db.execute("REPLACE INTO spin_gate VALUES(?,?,?,?,?,?,?,?,?,?,?,?)", row)
        n += 1
    return n


def load_measure(db, tsv):
    n = 0
    for line in open(tsv, encoding="utf-8", errors="replace"):
        if line.startswith("#") or not line.strip():
            continue
        p = line.rstrip("\n").split("\t")
        if len(p) < 8:
            continue
        db.execute("REPLACE INTO measurement VALUES(?,?,?,?,?,?,?,?,?)",
                   (p[0], p[1], int(p[2]), int(p[3]), int(p[4]), float(p[5]),
                    p[6], p[7], now()))
        n += 1
    return n


def report(db):
    queries = (
        ("sound_survey", "SELECT source_set, COUNT(*) FROM sound_survey GROUP BY 1"),
        ("spin_sweep", "SELECT source_set, COUNT(*) FROM spin_sweep GROUP BY 1"),
        ("spin_gate", "SELECT source_set || '/' || verdict, COUNT(*) FROM spin_gate GROUP BY 1"),
        ("measurement", "SELECT game, COUNT(*) FROM measurement GROUP BY 1"),
    )
    for name, q in queries:
        for src, c in db.execute(q):
            print(f"  {name:14s} {src:20s} {c}")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    db = sqlite3.connect(DB)
    db.executescript(SCHEMA)
    cmd = sys.argv[1]
    if cmd == "sound":
        print(f"loaded {load_sound(db, sys.argv[2], sys.argv[3])} rows")
    elif cmd == "spin":
        print(f"loaded {load_spin(db, sys.argv[2], sys.argv[3])} rows")
    elif cmd == "gate":
        print(f"loaded {load_gate(db, sys.argv[2], sys.argv[3])} rows")
    elif cmd == "measure":
        print(f"loaded {load_measure(db, sys.argv[2])} rows")
    elif cmd == "report":
        report(db)
    else:
        print(__doc__)
        return 1
    db.commit()
    return 0


if __name__ == "__main__":
    sys.exit(main())
