#!/usr/bin/env python3
"""Compare two sets of PICA probe results (e.g. real hardware vs an emulator).

    probe_compare.py REFERENCE_DIR OTHER_DIR [--tolerance N] [--report out.md]

Each directory holds the CSVs written by examples/probe (sdmc:/pica_probe).
For every experiment present in both, samples are matched by index and their
primary (pr..pa) and secondary (sr..sa) outputs compared channel by channel.
Differences larger than the tolerance are listed together with the sample's
parameters, which is what you need to work out the actual hardware rule.

Exit status: 0 if everything matches, 1 if any experiment differs.
"""
import argparse
import csv
import os
import sys

CHANNELS = ["pr", "pg", "pb", "pa", "sr", "sg", "sb", "sa"]


def load(path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    return {int(r["sample"]): r for r in rows}


def params(row):
    return ", ".join("%s=%s" % (k, v) for k, v in row.items() if k != "sample" and k not in CHANNELS)


def fmt(row, prefix):
    return "(%s)" % ",".join(row[prefix + c] for c in "rgba")


def compare(ref_dir, other_dir, tolerance, max_listed):
    names = sorted(f[:-4] for f in os.listdir(ref_dir) if f.endswith(".csv"))
    results = []
    for name in names:
        other_path = os.path.join(other_dir, name + ".csv")
        if not os.path.exists(other_path):
            results.append((name, None, [], 0))
            continue
        ref, other = load(os.path.join(ref_dir, name + ".csv")), load(other_path)
        diffs = []
        worst = 0
        for i in sorted(ref):
            if i not in other:
                diffs.append((i, ref[i], None, None))
                continue
            d = max(abs(int(ref[i][c]) - int(other[i][c])) for c in CHANNELS)
            worst = max(worst, d)
            if d > tolerance:
                diffs.append((i, ref[i], other[i], d))
        results.append((name, len(ref), diffs, worst))

    lines = []
    lines.append("| experiment | samples | differing | max diff |")
    lines.append("|---|---|---|---|")
    for name, n, diffs, worst in results:
        if n is None:
            lines.append("| %s | - | missing | - |" % name)
        else:
            lines.append("| %s | %d | %d | %d |" % (name, n, len(diffs), worst))
    for name, n, diffs, worst in results:
        if not diffs:
            continue
        lines.append("")
        lines.append("### %s" % name)
        lines.append("")
        lines.append("| sample | parameters | reference primary | other primary | reference secondary | other secondary |")
        lines.append("|---|---|---|---|---|---|")
        for i, r, o, d in diffs[:max_listed]:
            if o is None:
                lines.append("| %d | %s | %s | missing | %s | missing |" % (i, params(r), fmt(r, "p"), fmt(r, "s")))
            else:
                lines.append("| %d | %s | %s | %s | %s | %s |" % (i, params(r), fmt(r, "p"), fmt(o, "p"),
                                                               fmt(r, "s"), fmt(o, "s")))
        if len(diffs) > max_listed:
            lines.append("")
            lines.append("... %d more" % (len(diffs) - max_listed))
    differs = any(n is None or diffs for _, n, diffs, _ in results)
    return "\n".join(lines) + "\n", differs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("reference")
    ap.add_argument("other")
    ap.add_argument("--tolerance", type=int, default=0, help="allowed per-channel difference (default 0)")
    ap.add_argument("--max-listed", type=int, default=20, help="differing samples listed per experiment")
    ap.add_argument("--report", help="also write the report to this file")
    args = ap.parse_args()

    report, differs = compare(args.reference, args.other, args.tolerance, args.max_listed)
    header = "# PICA probe comparison\n\nreference: `%s`\n\nother: `%s`\n\ntolerance: %d\n\n" % (
        args.reference, args.other, args.tolerance)
    for d, label in ((args.reference, "reference"), (args.other, "other")):
        meta = os.path.join(d, "meta.txt")
        if os.path.exists(meta):
            header += "%s meta: %s\n\n" % (label, open(meta).read().strip().replace("\n", ", "))
    sys.stdout.write(header + report)
    if args.report:
        with open(args.report, "w") as f:
            f.write(header + report)
    return 1 if differs else 0


if __name__ == "__main__":
    sys.exit(main())
