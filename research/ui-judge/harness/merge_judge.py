"""Merge a single-judge re-run back into a combined judging CSV.

  python merge_judge.py <base.csv> <patch.csv> <judge_name> [more.csv judge ...]

Rows for `judge_name` are replaced wholesale by the ones in the patch file;
every other judge's rows are left exactly as they were. Used when one judge has
to be re-run on its own -- groq's throughput forces the open-weight judge into
a separate pass, and a parser fix meant re-running one judge's pairwise.
"""

import csv
import sys


def load(path):
    with open(path, encoding="utf-8") as fh:
        r = csv.DictReader(fh)
        return r.fieldnames, list(r)


def main():
    base_path, rest = sys.argv[1], sys.argv[2:]
    cols, rows = load(base_path)

    while rest:
        patch_path, judge, rest = rest[0], rest[1], rest[2:]
        pcols, prows = load(patch_path)
        if pcols != cols:
            sys.exit(f"column mismatch: {patch_path}")
        before = len(rows)
        rows = [r for r in rows if r["judge"] != judge]
        keep = [r for r in prows if r["judge"] == judge]
        rows += keep
        print(f"{judge}: dropped {before - len(rows) + len(keep)} old rows, "
              f"added {len(keep)} from {patch_path}")

    # Stable order so two runs of this produce identical files.
    key = ("frame" if "frame" in cols else "build_a")
    rows.sort(key=lambda r: tuple(r.get(c, "") for c in
                                  (key, "state", "judge", "criterion")
                                  if c in cols))
    with open(base_path, "w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=cols)
        w.writeheader()
        w.writerows(rows)
    print(f"wrote {base_path}: {len(rows)} rows")


if __name__ == "__main__":
    main()
