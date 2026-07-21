#!/usr/bin/env python3
"""Turn a wai.tsv run (snes_survey_wai.c output) into a summary histogram for
all four dead-wait groups: WAI, HV/NMI poll, APU handshake poll, other tight
loop. Never dump the raw per-ROM rows to the caller -- this is the only thing
meant to leave the RPi5. See tools/snes_survey/README.md and remote_wai.sh."""
import sys

# (field name, label, TSV column index) -- columns are:
# 0=name 1=status 2=lit 3=WAI_DOT 4=WAI_TICK 5=HV_DOT 6=APU_DOT 7=OTHER_DOT
GROUPS = [("WAI_DOT", "WAI", 3), ("HV_DOT", "HV/NMI poll", 5),
          ("APU_DOT", "APU poll", 6), ("OTHER_DOT", "other tight loop", 7)]

def parse_pct(field, prefix):
    if not field.startswith(prefix):
        return None
    val = field[len(prefix):]
    if val in ("NA", ""):
        return None
    try:
        return float(val)
    except ValueError:
        return None

def histogram(label, values):
    buckets = [("0%", lambda d: d == 0.0),
               ("0-10%", lambda d: 0.0 < d < 10.0),
               ("10-20%", lambda d: 10.0 <= d < 20.0),
               ("20-40%", lambda d: 20.0 <= d < 40.0),
               ("40%+", lambda d: d >= 40.0)]
    n_total = len(values)
    print(f"-- {label} histogram (of {n_total} classifiable) --")
    for blabel, pred in buckets:
        n = sum(1 for d in values if pred(d))
        pct = 100.0 * n / n_total if n_total else 0.0
        print(f"  {blabel:8s}: {n:5d}  ({pct:5.1f}%)")
    n20 = sum(1 for d in values if d >= 20.0)
    n40 = sum(1 for d in values if d >= 40.0)
    if n_total:
        print(f"  >=20%: {n20} ({100.0*n20/n_total:.1f}%)   >=40%: {n40} ({100.0*n40/n_total:.1f}%)")
    print()

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "wai.tsv"
    total = 0
    status_counts = {}
    rows = []  # (name, {field: pct})
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            total += 1
            parts = line.split("\t")
            status = parts[1] if len(parts) > 1 else "UNKNOWN"
            status_counts[status] = status_counts.get(status, 0) + 1
            if status != "OK" or len(parts) < 8:
                continue
            name = parts[0]
            vals = {}
            ok = True
            for field, _, col in GROUPS:
                pct = parse_pct(parts[col], field + "=")
                if pct is None:
                    ok = False
                    break
                vals[field] = pct
            if ok:
                rows.append((name, vals))

    print(f"total rows: {total}")
    for st, c in sorted(status_counts.items(), key=lambda kv: -kv[1]):
        print(f"  {st}: {c}")
    print(f"classifiable (OK, all 4 groups measured): {len(rows)}")
    print()

    for field, label, _ in GROUPS:
        values = [v[field] for _, v in rows]
        histogram(label, values)

    for field, label, _ in GROUPS:
        top = sorted(rows, key=lambda r: -r[1][field])[:10]
        print(f"Top 10 {label}-heavy:")
        for name, v in top:
            print(f"  {v[field]:5.1f}%  {name}  (WAI={v['WAI_DOT']:.1f} HV={v['HV_DOT']:.1f} "
                  f"APU={v['APU_DOT']:.1f} OTHER={v['OTHER_DOT']:.1f})")
        print()

if __name__ == "__main__":
    main()
