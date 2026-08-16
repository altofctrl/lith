"""X.5 Analysis. Reads every gate's CSV and writes the result tables.

  python analyze.py <results_dir>

Runs on the anaconda python (numpy/pandas/scipy). Everything it prints is
derived from the CSVs in results_dir; nothing is hard-coded from an earlier run.
"""

import itertools
import json
import os
import sys

import numpy as np
import pandas as pd

TIER_ORDER = {"low": 0, "mid": 1, "high": 2}
FACTORS = ["ease_of_use", "clarity", "visual_hierarchy", "memorable", "trust",
           "intuitive", "aesthetic_pleasure", "interest", "comfort"]
STATES = ["1_boot_idle", "2_started", "3_mid_meeting",
          "4_threshold", "5_extended", "6_stopped"]


def krippendorff_ordinal(matrix):
    """Krippendorff's alpha for ordinal data.

    matrix: units x coders, np.nan where a coder did not score that unit.

    Written out rather than pulled from a package because the reliability floor
    is load-bearing here -- X.5 says low agreement invalidates the absolute
    scores -- and a number that decides that should be inspectable.
    """
    m = np.asarray(matrix, dtype=float)
    # Only units scored by at least two coders contribute.
    counts = (~np.isnan(m)).sum(axis=1)
    m = m[counts >= 2]
    if m.size == 0:
        return float("nan")

    vals = np.unique(m[~np.isnan(m)])
    if vals.size < 2:
        return float("nan")
    # ordinal metric: squared distance summed over the ranks between two values
    n_v = {v: float(np.sum(m == v)) for v in vals}
    n_total = sum(n_v.values())

    def delta2(a, b):
        lo, hi = (a, b) if a <= b else (b, a)
        between = [v for v in vals if lo <= v <= hi]
        s = sum(n_v[v] for v in between) - (n_v[lo] + n_v[hi]) / 2.0
        return s ** 2

    # observed disagreement
    do_num, do_den = 0.0, 0.0
    for row in m:
        row = row[~np.isnan(row)]
        mu = len(row)
        if mu < 2:
            continue
        for a, b in itertools.permutations(row, 2):
            do_num += delta2(a, b) / (mu - 1)
        do_den += mu
    if do_den == 0:
        return float("nan")
    Do = do_num / do_den

    # expected disagreement
    de = 0.0
    for a in vals:
        for b in vals:
            if a == b:
                continue
            de += n_v[a] * n_v[b] * delta2(a, b)
    De = de / (n_total * (n_total - 1))
    return 1.0 - Do / De if De else float("nan")


def jonckheere(groups):
    """Jonckheere-Terpstra trend statistic, normal approximation.

    groups: list of arrays, in the hypothesised increasing order.
    Returns (J, z, p_two_sided).
    """
    from math import erfc, sqrt
    groups = [np.asarray(g, dtype=float) for g in groups]
    groups = [g[~np.isnan(g)] for g in groups]
    if sum(len(g) > 0 for g in groups) < 2:
        return float("nan"), float("nan"), float("nan")
    J = 0.0
    for i in range(len(groups)):
        for j in range(i + 1, len(groups)):
            for x in groups[i]:
                J += np.sum(groups[j] > x) + 0.5 * np.sum(groups[j] == x)
    ns = np.array([len(g) for g in groups], dtype=float)
    N = ns.sum()
    mu = (N ** 2 - np.sum(ns ** 2)) / 4.0
    var = (N ** 2 * (2 * N + 3) - np.sum(ns ** 2 * (2 * ns + 3))) / 72.0
    if var <= 0:
        return J, float("nan"), float("nan")
    z = (J - mu) / sqrt(var)
    p = erfc(abs(z) / sqrt(2))
    return J, z, p


# Mean seconds per conversational turn, from the `latency` sheet of
# lith_provider_latency.xlsx (3 reps x 3 turns per model). Copied in rather than
# re-measured: X.5 asks for these results *joined* to the existing benchmark,
# not for a fresh timing run.
LATENCY_S = {
    "llama-3.1-8b-instant": 0.366,
    "llama-3.3-70b-versatile": 0.762,
    "openai/gpt-oss-120b": 1.170,
    "gpt-4o-mini": 1.313,
    "gpt-4.1": 1.174,
    "gpt-5.5": 0.193,
    "claude-haiku-4-5-20251001": 2.429,
    "claude-sonnet-5": 39.087,
    "claude-opus-5": 49.427,
    "deepseek-v4-flash": 73.968,
    "deepseek-v4-pro": 21.495,
}


def tier_of(build_id, gate1):
    row = gate1[gate1.build_id == build_id]
    return row.tier.iloc[0] if len(row) else "?"


def main():
    d = sys.argv[1]
    out = []

    def say(*a):
        line = " ".join(str(x) for x in a)
        print(line)
        out.append(line)

    gate1 = pd.read_csv(os.path.join(d, "gate1_compile.csv"))
    repair = pd.read_csv(os.path.join(d, "gate1_repair.csv"))
    render = pd.read_csv(os.path.join(d, "gate2_render_deployed.csv"))
    absolute = pd.read_csv(os.path.join(d, "judge_absolute.csv"))
    pair_path = os.path.join(d, "judge_pairwise.csv")
    pair = pd.read_csv(pair_path) if os.path.exists(pair_path) else None

    key = gate1[["build_id", "provider", "tier", "model", "rep"]]

    # ---------------------------------------------------------- Gate 1
    say("\n=== Gate 1: compile ===")
    g = gate1.merge(repair[["build_id", "passes_used", "deployed_compiled"]],
                    on="build_id", how="left")
    t = g.groupby(["provider", "tier"]).agg(
        n=("build_id", "count"),
        first_pass=("compiled", "sum"),
        as_deployed=("deployed_compiled", "sum")).reset_index()
    t["tier_i"] = t.tier.map(TIER_ORDER)
    say(t.sort_values(["provider", "tier_i"]).drop(columns="tier_i").to_string(index=False))
    say(f"\noverall first-pass  {int(g.compiled.sum())}/{len(g)} "
        f"= {g.compiled.mean():.0%}")
    say(f"overall as-deployed {int(g.deployed_compiled.sum())}/{len(g)} "
        f"= {g.deployed_compiled.mean():.0%}")
    by_tier = g.groupby("tier").agg(first=("compiled", "mean"),
                                    deployed=("deployed_compiled", "mean"))
    say("\nby tier (pooled across providers):")
    say(by_tier.reindex(["low", "mid", "high"]).to_string())

    # ---------------------------------------------------------- Gates 2/3
    say("\n=== Gates 2-3: render and journey coverage ===")
    r = render.merge(key, on="build_id", how="left")
    say(f"sim build ok      {int(r.sim_build_ok.sum())}/{len(r)}")
    say(f"ran to completion {int(r.ran_ok.sum())}/{len(r)}")
    ok = r[r.ran_ok == True]  # noqa: E712
    if len(ok):
        cov = ok.groupby(["provider", "tier"]).agg(
            n=("build_id", "count"),
            frames=("frames_present", "mean"),
            distinct=("distinct_frames", "mean"),
            blank=("blank_frames", "sum"),
            rot_set=("rotation_set", "sum")).reset_index()
        cov["tier_i"] = cov.tier.map(TIER_ORDER)
        say(cov.sort_values(["provider", "tier_i"]).drop(columns="tier_i")
            .to_string(index=False))
        say("\ndistinct-frame coverage by tier (of 6):")
        say(ok.groupby("tier").distinct_frames.agg(["mean", "std", "count"])
            .reindex(["low", "mid", "high"]).to_string())
        j = jonckheere([ok[ok.tier == t].distinct_frames.values
                        for t in ["low", "mid", "high"]])
        say(f"Jonckheere-Terpstra trend on coverage: J={j[0]:.1f} z={j[1]:.2f} p={j[2]:.3f}")

    # ---------------------------------------------------------- reliability
    say("\n=== Inter-judge reliability (Krippendorff's alpha, ordinal) ===")
    say("Unit of analysis = one frame. Alpha is computed per factor across the")
    say("three judges' medians. X.5: low agreement invalidates the absolute scores.")
    alphas = {}
    for f in FACTORS:
        piv = absolute.pivot_table(index="frame", columns="judge", values=f,
                                   aggfunc="median")
        alphas[f] = krippendorff_ordinal(piv.values)
    a_ser = pd.Series(alphas).sort_values()
    say(a_ser.to_string(float_format=lambda x: f"{x: .3f}"))
    say(f"\nmean alpha across factors: {a_ser.mean():.3f}")

    # ---------------------------------------------------------- absolute
    say("\n=== Absolute Likert scores by tier (descriptive) ===")
    a = absolute.merge(key, on="build_id", how="left")
    a = a[a.tier.isin(TIER_ORDER)]
    for judge in sorted(a.judge.unique()):
        sub = a[a.judge == judge]
        say(f"\n-- judge: {judge}")
        tab = sub.groupby("tier")[FACTORS].agg(["mean", "std"])
        mean_only = sub.groupby("tier")[FACTORS].mean().reindex(["low", "mid", "high"])
        say(mean_only.to_string(float_format=lambda x: f"{x:.2f}"))
        # Builds as units, not frames: the six frames of one build are six views
        # of one design, so testing over frames would claim n=90 for a study
        # with n=15 and turn a modest tier gap into a tiny p-value.
        pb = sub.groupby(["build_id", "tier"], as_index=False)[FACTORS].mean()
        trends = {}
        for f in FACTORS:
            _, z, p = jonckheere([pb[pb.tier == t][f].dropna().values
                                  for t in ["low", "mid", "high"]])
            trends[f] = (z, p)
        say("ordinal trend low<mid<high, builds as units (n="
            f"{len(pb)}): " + ", ".join(
                f"{f}: z={z:+.2f} p={p:.3f}" for f, (z, p) in trends.items()))

    say("\n-- pooled across judges, mean of the nine factors")
    a["mean9"] = a[FACTORS].mean(axis=1)
    say(a.groupby(["tier"]).mean9.agg(["mean", "std", "count"])
        .reindex(["low", "mid", "high"]).to_string())

    # The unit of analysis is the build, not the frame. Six frames from one
    # build are six views of one design and are not independent draws; running
    # the trend test over frames would treat n=90 where the study really has
    # n=15, and would report a confidence the design cannot support.
    per_build = (a.groupby(["build_id", "provider", "tier"], as_index=False)
                  .mean9.mean())
    say("\n-- per build (the unit of analysis), mean9 averaged over its frames "
        "and judges")
    say(per_build.sort_values(["provider", "tier"]).to_string(
        index=False, float_format=lambda x: f"{x:.2f}"))
    say("\nby tier, builds as units:")
    say(per_build.groupby("tier").mean9.agg(["mean", "std", "count"])
        .reindex(["low", "mid", "high"]).to_string())
    _, z, p = jonckheere([per_build[per_build.tier == t].mean9.values
                          for t in ["low", "mid", "high"]])
    say(f"Jonckheere-Terpstra on per-build mean9: z={z:+.2f} p={p:.3f}  (n="
        f"{len(per_build)} builds)")
    _, zf, pf = jonckheere([a[a.tier == t].mean9.dropna().values
                            for t in ["low", "mid", "high"]])
    say(f"  (same test over frames, which are nested and so overstate n: "
        f"z={zf:+.2f} p={pf:.3f} -- reported only to show the inflation)")

    say("\n-- by provider and tier (mean of nine factors, pooled judges)")
    pt = a.pivot_table(index="provider", columns="tier", values="mean9",
                       aggfunc="mean")
    say(pt.reindex(columns=["low", "mid", "high"]).to_string(
        float_format=lambda x: f"{x:.2f}"))

    say("\n-- by journey state (pooled)")
    st = a.pivot_table(index="state", columns="tier", values="mean9", aggfunc="mean")
    say(st.reindex(columns=["low", "mid", "high"]).to_string(
        float_format=lambda x: f"{x:.2f}"))

    # ---------------------------------------------------------- pairwise
    if pair is not None and len(pair):
        say("\n=== Within-provider pairwise (primary evidence) ===")
        pair = pair.copy()
        pair["tier_a"] = pair.build_a.map(lambda b: tier_of(b, gate1))
        pair["tier_b"] = pair.build_b.map(lambda b: tier_of(b, gate1))
        pair["cross_tier"] = pair.tier_a != pair.tier_b
        say(f"comparisons: {len(pair)}  "
            f"position-consistent: {pair.consistent.mean():.0%}")
        say("(inconsistent = the judge flipped when the two were swapped; "
            "counted as a tie, never resolved)")

        ct = pair[pair.cross_tier & (pair.winner != "tie")].copy()
        if len(ct):
            ct["higher_tier_won"] = [
                TIER_ORDER.get(r.tier_a, -1) > TIER_ORDER.get(r.tier_b, -1)
                if r.winner == r.build_a else
                TIER_ORDER.get(r.tier_b, -1) > TIER_ORDER.get(r.tier_a, -1)
                for r in ct.itertuples()]
            say("\nhigher-tier win rate on cross-tier comparisons, per judge:")
            say(ct.groupby(["judge", "criterion"]).higher_tier_won.mean()
                .unstack().to_string(float_format=lambda x: f"{x:.2f}"))
            say("\npooled over judges:")
            say(ct.groupby("criterion").higher_tier_won.agg(["mean", "count"])
                .to_string(float_format=lambda x: f"{x:.2f}"))
            say(f"\noverall higher-tier win rate: {ct.higher_tier_won.mean():.1%} "
                f"(n={len(ct)}; 50% = no tier effect)")
            say("\ntie rate by judge (position flips, i.e. no stable preference):")
            say(pair[pair.cross_tier].groupby("judge").winner
                .apply(lambda s: (s == "tie").mean())
                .to_string(float_format=lambda x: f"{x:.2f}"))

    # ------------------------------------------------- quality per second
    say("\n=== Quality against latency ===")
    say("Joined to the mean seconds/turn in lith_provider_latency.xlsx. Read as")
    say("a cost-of-quality ratio, not as a score: it is only meaningful if the")
    say("tier effect below is real, and the reliability floor above says whether")
    say("the quality term can carry that weight at all.")
    pm = (a.groupby(["model", "provider", "tier"], as_index=False).mean9.mean())
    # The build_id encodes `openai/gpt-oss-120b` as `openai_gpt-oss-120b`,
    # because a slash cannot go in a filename; the latency sheet uses the real
    # model id.
    pm["mean_s_per_turn"] = pm.model.str.replace("openai_gpt-oss",
                                                 "openai/gpt-oss",
                                                 regex=False).map(LATENCY_S)
    pm["mean9_per_s"] = pm.mean9 / pm.mean_s_per_turn
    pm["tier_i"] = pm.tier.map(TIER_ORDER)
    say(pm.sort_values(["provider", "tier_i"])
        .drop(columns="tier_i")
        .to_string(index=False, float_format=lambda x: f"{x:.3f}"))
    say("\nNo cost column: the raw benchmark sheet records no token counts, and")
    say("list prices for the full resolved matrix are not written down anywhere")
    say("in the existing artefacts. Quality-per-dollar needs a live price check")
    say("for all ten model-tiers first, so it is left undone rather than guessed.")

    with open(os.path.join(d, "analysis.txt"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(out) + "\n")
    print("\nwrote", os.path.join(d, "analysis.txt"))


if __name__ == "__main__":
    main()
