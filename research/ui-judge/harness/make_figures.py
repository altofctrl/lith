#!/usr/bin/env python3
"""
Report figures for the co-design agent section.

Unlike make_artifact.py (which builds the shareable HTML), this writes
publication figures at fixed physical sizes into figures/.

    python harness/make_figures.py .

Figure 1 is laid out so that at dpi=200 each panel is exactly 320x170 px,
the device's native resolution, so the frames are never resampled.
"""
import sys, os
import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle, FancyArrowPatch
import matplotlib.image as mpimg
import matplotlib.patheffects as pe

# ---------------------------------------------------------------- design system
BG      = "#faf9f7"
INK     = "#1b1b1b"
MUTED   = "#6f6f6f"
FAINT   = "#d8d5d0"
ACCENT  = "#c8551f"          # same annotation orange as the onboarding figures

TIER_C  = {"low": "#9fb3c8", "mid": "#4a7ba7", "high": "#1f4b73"}
TIER_LB = {"low": "low", "mid": "mid", "high": "high"}
PROV_C  = {"groq": "#2f7de1", "openai": "#e2622a",
           "anthropic": "#5540b8", "deepseek": "#18a06e"}

STATES  = ["1_boot_idle", "2_started", "3_mid_meeting",
           "4_threshold", "5_extended", "6_stopped"]
STATE_T = ["1 · boot / idle", "2 · started", "3 · mid-meeting",
           "4 · threshold", "5 · extended", "6 · stopped"]

FACTORS = ["ease_of_use", "clarity", "visual_hierarchy", "memorable", "trust",
           "intuitive", "aesthetic_pleasure", "interest", "comfort"]
FACT_T  = {"ease_of_use": "ease of use", "clarity": "clarity",
           "visual_hierarchy": "visual hierarchy", "memorable": "memorable",
           "trust": "trust", "intuitive": "intuitive",
           "aesthetic_pleasure": "aesthetic pleasure", "interest": "interest",
           "comfort": "comfort", "overall": "overall"}

plt.rcParams.update({
    "font.family": "sans-serif",
    "font.sans-serif": ["Segoe UI", "Lato", "Open Sans", "DejaVu Sans"],
    "figure.facecolor": BG, "axes.facecolor": BG,
    "savefig.facecolor": BG, "text.color": INK,
    "axes.edgecolor": FAINT, "axes.labelcolor": INK,
    "xtick.color": MUTED, "ytick.color": MUTED,
    "axes.titlecolor": INK, "pdf.fonttype": 42,
})

# list API pricing, $ per 1M tokens, as charted in lith_provider_cost.png
# (checked live 2026-08-10; input, output)
PRICE = {
    "llama-3.1-8b-instant":      (0.05,  0.08),
    "llama-3.3-70b-versatile":   (0.59,  0.79),
    "openai_gpt-oss-120b":       (0.15,  0.60),
    "gpt-4o-mini":               (0.15,  0.60),
    "gpt-4.1":                   (2.00,  8.00),
    "gpt-5.5":                   (5.00, 30.00),
    "claude-haiku-4-5-20251001": (1.00,  5.00),
    "claude-sonnet-5":           (3.00, 15.00),
    "claude-opus-5":             (5.00, 25.00),
    "deepseek-v4-flash":         (0.14,  0.28),
    "deepseek-v4-pro":           (0.435, 0.87),
}
# mean seconds/turn, lith_provider_latency.xlsx, 'latency' sheet
LAT = {
    "llama-3.1-8b-instant": 0.366, "llama-3.3-70b-versatile": 0.762,
    "openai_gpt-oss-120b": 1.170, "gpt-4o-mini": 1.313, "gpt-4.1": 1.174,
    "gpt-5.5": 0.193, "claude-haiku-4-5-20251001": 2.429,
    "claude-sonnet-5": 39.087, "claude-opus-5": 49.427,
    "deepseek-v4-flash": 73.968, "deepseek-v4-pro": 21.495,
}
SHORT = {
    "claude-haiku-4-5-20251001": "claude-haiku-4-5",
    "openai_gpt-oss-120b": "gpt-oss-120b",
    "llama-3.1-8b-instant": "llama-3.1-8b",
    "llama-3.3-70b-versatile": "llama-3.3-70b",
}

TIER_ORDER = {"low": 0, "mid": 1, "high": 2}


def load(root):
    r = os.path.join(root, "results")
    d = {}
    d["g1"] = pd.read_csv(os.path.join(r, "gate1_compile.csv"))
    d["rep"] = pd.read_csv(os.path.join(r, "gate1_repair.csv"))
    d["abs"] = pd.read_csv(os.path.join(r, "judge_absolute.csv"))
    d["pair"] = pd.read_csv(os.path.join(r, "judge_pairwise.csv"))
    d["g2"] = pd.read_csv(os.path.join(r, "gate2_render_deployed.csv"))
    return d


def build_table(d):
    """One row per judged build: provider, tier, model, rep, mean9."""
    a = d["abs"].copy()
    a["mean9"] = a[FACTORS].mean(axis=1)
    per_build = a.groupby("build_id")["mean9"].mean()
    meta = d["rep"].set_index("build_id")[["provider", "tier", "model", "rep"]]
    t = meta.join(per_build, how="inner").dropna(subset=["mean9"]).reset_index()
    # groq reports "openai/gpt-oss-120b"; build_ids use the "_" form
    t["model"] = t["model"].str.replace("/", "_", regex=False)
    t["torder"] = t["tier"].map(TIER_ORDER)
    return t


# ------------------------------------------------------------------- figure 1
def fig1(root, tbl, d, out):
    """The tier ladder in pixels: every judged build x every journey state."""
    frames = os.path.join(root, "frames", "deployed")
    t = tbl.sort_values(["torder", "mean9"], ascending=[True, False])
    rows = list(t.itertuples())
    n = len(rows)

    blanks = d["g2"].set_index("build_id")["blank_frames"].to_dict()

    # inches
    L, R = 2.25, 1.55
    CW, GAP = 1.60, 0.055
    CH = CW * 170 / 320
    TOP, BOT, TGAP = 0.92, 0.42, 0.20
    ngaps = 2
    W = L + 6 * CW + 5 * GAP + R
    H = TOP + n * CH + (n - 1) * GAP + ngaps * TGAP + BOT

    fig = plt.figure(figsize=(W, H))

    def ax_at(x, y, w, h):
        return fig.add_axes([x / W, 1 - (y + h) / H, w / W, h / H])

    # column headers
    for c, lab in enumerate(STATE_T):
        x = L + c * (CW + GAP)
        fig.text((x + CW / 2) / W, 1 - (TOP - 0.30) / H, lab, ha="center",
                 va="bottom", fontsize=9.5, color=MUTED)

    fig.text(L / W, 1 - 0.30 / H, "Figure 1", ha="left", va="bottom",
             fontsize=13, fontweight="bold", color=INK)
    fig.text((L + 1.05) / W, 1 - 0.315 / H,
             "every deployed build, driven down the same six-state journey — "
             "the device's own pixels, at its native 320×170",
             ha="left", va="bottom", fontsize=10.5, color=MUTED)

    # callouts: (build_id, state, letter)
    calls = {
        ("anthropic_claude-haiku-4-5-20251001_rep2", "2_started"): "i",
        ("openai_gpt-4o-mini_rep1", "4_threshold"): "ii",
        ("anthropic_claude-opus-5_rep1", "4_threshold"): "iii",
    }

    y = TOP
    prev_tier = None
    tier_spans = {}
    for r in rows:
        if prev_tier is not None and r.tier != prev_tier:
            y += TGAP
        tier_spans.setdefault(r.tier, [y, y])
        prev_tier = r.tier

        for c, st in enumerate(STATES):
            x = L + c * (CW + GAP)
            ax = ax_at(x, y, CW, CH)
            p = os.path.join(frames, f"{r.build_id}__{st}.png")
            if os.path.exists(p):
                ax.imshow(mpimg.imread(p), interpolation="nearest", aspect="auto")
            else:
                ax.set_facecolor("#efece7")
            ax.set_xticks([]); ax.set_yticks([])
            for s in ax.spines.values():
                s.set_color(FAINT); s.set_linewidth(0.6)
            key = (r.build_id, st)
            if key in calls:
                ax.add_patch(Rectangle((0, 0), 1, 1, transform=ax.transAxes,
                                       fill=False, ec=ACCENT, lw=2.0, zorder=6))
                ax.text(0.965, 0.90, f"({calls[key]})", transform=ax.transAxes,
                        ha="right", va="center", fontsize=9, color="#ffffff",
                        fontweight="bold", zorder=7,
                        bbox=dict(boxstyle="round,pad=0.20", fc=ACCENT, ec="none"))

        # left label
        model = SHORT.get(r.model, r.model)
        fig.text((L - 0.13) / W, 1 - (y + CH / 2 + 0.055) / H, model,
                 ha="right", va="center", fontsize=9.2, color=INK)
        fig.text((L - 0.13) / W, 1 - (y + CH / 2 - 0.10) / H,
                 f"{r.provider} · rep {r.rep}", ha="right", va="center",
                 fontsize=7.8, color=MUTED)

        # right: mean9 bar
        bx = L + 6 * CW + 5 * GAP + 0.16
        bax = ax_at(bx, y + CH / 2 - 0.085, 0.80, 0.17)
        bax.set_xlim(1, 7); bax.set_ylim(0, 1)
        bax.axis("off")
        bax.add_patch(Rectangle((1, 0.12), 6, 0.76, fc="#eceae6", ec="none"))
        bax.add_patch(Rectangle((1, 0.12), r.mean9 - 1, 0.76,
                                fc=TIER_C[r.tier], ec="none"))
        nb = blanks.get(r.build_id, 0)
        lab = f"{r.mean9:.2f}"
        fig.text((bx + 0.88) / W, 1 - (y + CH / 2) / H, lab, ha="left",
                 va="center", fontsize=9.2, color=INK)
        if nb:
            fig.text((bx + 0.88) / W, 1 - (y + CH / 2 + 0.145) / H,
                     f"{nb} blank", ha="left", va="center", fontsize=7.2,
                     color=ACCENT)
        y += CH + GAP

    # tier bands down the far left
    for tier, (y0, y1) in tier_spans.items():
        y1 = y1 + CH
        ax = ax_at(0.16, y0, 0.085, y1 - y0)
        ax.set_facecolor(TIER_C[tier]); ax.set_xticks([]); ax.set_yticks([])
        for s in ax.spines.values():
            s.set_visible(False)
        fig.text(0.10 / W, 1 - ((y0 + y1) / 2) / H, TIER_LB[tier].upper(),
                 ha="center", va="center", rotation=90, fontsize=9.5,
                 color=TIER_C[tier], fontweight="bold")

    fig.text(L / W, (BOT - 0.30) / H,
             "score column: mean of the nine factors, pooled over three judges "
             "and six states, on the judges' 1-to-7 scale",
             ha="left", va="center", fontsize=8.6, color=MUTED)
    fig.text((L + 6 * CW + 5 * GAP + 0.16) / W, 1 - (TOP - 0.30) / H,
             "judged quality", ha="left", va="bottom", fontsize=9.5, color=MUTED)

    fig.savefig(out, dpi=200)
    plt.close(fig)
    print("wrote", out, f"({n} builds)")


# ------------------------------------------------------------------- figure 2
def fig2(root, d, out):
    """Gate 1: the repair loop is the largest effect in the study."""
    g1 = d["g1"]; rp = d["rep"]
    m = g1[["build_id", "provider", "tier", "model", "compiled"]].merge(
        rp[["build_id", "deployed_compiled", "passes_used"]], on="build_id")
    m["torder"] = m["tier"].map(TIER_ORDER)

    fig, (ax, ax2) = plt.subplots(
        1, 2, figsize=(11.6, 4.5), gridspec_kw={"width_ratios": [1.85, 1]})
    fig.subplots_adjust(left=0.075, right=0.975, top=0.76, bottom=0.14, wspace=0.28)

    fig.text(0.075, 0.955, "Figure 2", fontsize=13, fontweight="bold", ha="left")
    fig.text(0.075, 0.885,
             "The hidden repair loop, not the model tier, is what decides whether a "
             "workshop participant gets working firmware",
             fontsize=10.5, color=MUTED, ha="left")

    tiers = ["low", "mid", "high"]
    fp, dep, ns = [], [], []
    for t in tiers:
        s = m[m["tier"] == t]
        ns.append(len(s))
        fp.append(s["compiled"].mean() * 100)
        dep.append(s["deployed_compiled"].mean() * 100)
    allfp = m["compiled"].mean() * 100
    alldep = m["deployed_compiled"].mean() * 100

    x = np.arange(len(tiers) + 1)
    fpv = fp + [allfp]; depv = dep + [alldep]
    labs = [f"{TIER_LB[t]}\nn={n}" for t, n in zip(tiers, ns)] + [f"all\nn={len(m)}"]

    for i, (a, b) in enumerate(zip(fpv, depv)):
        ax.plot([i, i], [a, b], color=FAINT, lw=2.2, zorder=1, solid_capstyle="round")
        ax.annotate("", xy=(i, b - 1.6), xytext=(i, a + 1.6),
                    arrowprops=dict(arrowstyle="-|>", color=ACCENT, lw=1.7,
                                    shrinkA=0, shrinkB=0), zorder=2)
        ax.scatter([i], [a], s=110, color="#ffffff", ec=MUTED, lw=1.6, zorder=3)
        ax.scatter([i], [b], s=130, color=ACCENT, ec="none", zorder=3)
        ax.text(i + 0.13, a, f"{a:.0f}%", va="center", ha="left",
                fontsize=9.5, color=MUTED)
        ax.text(i + 0.13, b, f"{b:.0f}%", va="center", ha="left",
                fontsize=10, color=ACCENT, fontweight="bold")

    ax.axvline(2.5, color=FAINT, lw=1, ls=(0, (3, 3)))
    ax.set_xticks(x); ax.set_xticklabels(labs, fontsize=9.5, color=INK)
    ax.set_ylim(-6, 112); ax.set_xlim(-0.5, len(x) - 0.3)
    ax.set_ylabel("builds that compile  (%)", fontsize=9.5)
    ax.set_yticks([0, 25, 50, 75, 100])
    ax.grid(axis="y", color=FAINT, lw=0.7); ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    ax.scatter([], [], s=110, color="#ffffff", ec=MUTED, lw=1.6,
               label="first pass  (what the model wrote)")
    ax.scatter([], [], s=130, color=ACCENT, label="as deployed  (after builder.py repairs)")
    ax.legend(loc="upper left", frameon=False, fontsize=9, handletextpad=0.5,
              bbox_to_anchor=(0.0, 0.99))

    # right panel: how many passes each rescue took
    resc = m[(~m["compiled"]) & (m["deployed_compiled"])]
    lost = m[(~m["compiled"]) & (~m["deployed_compiled"])]
    counts = resc["passes_used"].value_counts().sort_index()
    bars = [int(counts.get(2, 0)), int(counts.get(3, 0)), len(lost)]
    cols = [ACCENT, "#e0a98d", "#c9c4bc"]
    names = ["rescued on\npass 2", "rescued on\npass 3", "never\ncompiled"]
    ax2.bar(range(3), bars, color=cols, width=0.62)
    for i, v in enumerate(bars):
        ax2.text(i, v + 0.25, str(v), ha="center", va="bottom",
                 fontsize=11, fontweight="bold",
                 color=INK if v else MUTED)
    ax2.set_xticks(range(3)); ax2.set_xticklabels(names, fontsize=9, color=INK)
    ax2.set_ylim(0, max(bars) + 2.2)
    ax2.set_ylabel("builds that failed first pass", fontsize=9.5)
    ax2.grid(axis="y", color=FAINT, lw=0.7); ax2.set_axisbelow(True)
    for s in ("top", "right"):
        ax2.spines[s].set_visible(False)
    ax2.set_title("MAX_ATTEMPTS = 3 bought nothing", fontsize=9.5,
                  color=MUTED, pad=8, loc="left")

    fig.savefig(out, dpi=200)
    plt.close(fig)
    print("wrote", out)


# ------------------------------------------------------------------- figure 3
def fig3(root, tbl, d, out):
    """What the judges actually separated, and how far to trust it."""
    pair = d["pair"].copy()
    tiers = tbl.set_index("build_id")["tier"].to_dict()
    pair["ta"] = pair["build_a"].map(tiers)
    pair["tb"] = pair["build_b"].map(tiers)
    pair = pair.dropna(subset=["ta", "tb"])
    pair = pair[pair["ta"] != pair["tb"]]

    def hi_win(row):
        hi = row["build_a"] if TIER_ORDER[row["ta"]] > TIER_ORDER[row["tb"]] else row["build_b"]
        if not row["consistent"]:
            return np.nan
        return 1.0 if row["winner"] == hi else 0.0

    pair["hi"] = pair.apply(hi_win, axis=1)
    p = pair.dropna(subset=["hi"])

    order = (p.groupby("criterion")["hi"].mean().sort_values())
    crits = list(order.index)
    judges = ["claude-sonnet-5", "gpt-4.1", "qwen3.6-27b"]
    jmark = {"claude-sonnet-5": "o", "gpt-4.1": "s", "qwen3.6-27b": "^"}

    fig = plt.figure(figsize=(12.4, 6.2))
    gs = fig.add_gridspec(1, 3, width_ratios=[1.5, 1.0, 0.78],
                          left=0.115, right=0.975, top=0.74, bottom=0.175,
                          wspace=0.44)
    axA = fig.add_subplot(gs[0]); axB = fig.add_subplot(gs[1]); axC = fig.add_subplot(gs[2])

    fig.text(0.031, 0.945, "Figure 3", fontsize=13, fontweight="bold", ha="left")
    fig.text(0.031, 0.875,
             "The judges separate the tiers on visual hierarchy, and barely at all on "
             "anything a still frame cannot show",
             fontsize=10.5, color=MUTED, ha="left")

    # A — pairwise win rate by criterion
    for i, c in enumerate(crits):
        pooled = order[c]
        axA.plot([0.5, pooled], [i, i], color=FAINT, lw=1.4, zorder=1)
        for j in judges:
            v = p[(p["criterion"] == c) & (p["judge"] == j)]["hi"].mean()
            axA.scatter([v], [i], marker=jmark[j], s=42, color=MUTED,
                        alpha=0.65, zorder=2, lw=0)
        axA.scatter([pooled], [i], s=105, color=ACCENT, zorder=3,
                    ec=BG, lw=1.2)
        axA.text(1.035, i, f"{pooled*100:.0f}%", va="center",
                 ha="right", fontsize=9, color=INK,
                 fontweight="bold" if c == "visual_hierarchy" else "normal")
    axA.axvline(0.5, color=INK, lw=1.1)
    axA.text(0.5, len(crits) - 0.35, "no tier effect", fontsize=8.4,
             color=MUTED, ha="center", va="bottom",
             bbox=dict(boxstyle="round,pad=0.2", fc=BG, ec="none"))
    axA.set_yticks(range(len(crits)))
    axA.set_yticklabels([FACT_T[c] for c in crits], fontsize=9.5, color=INK)
    for t, c in zip(axA.get_yticklabels(), crits):
        if c == "visual_hierarchy":
            t.set_color(ACCENT); t.set_fontweight("bold")
    axA.set_xlim(0.45, 1.045); axA.set_ylim(-0.6, len(crits) - 0.1)
    axA.set_xticks([0.5, 0.6, 0.7, 0.8, 0.9, 1.0])
    axA.set_xticklabels(["50%", "60%", "70%", "80%", "90%", "100%"], fontsize=9)
    axA.set_xlabel("higher tier preferred, within provider", fontsize=9.5)
    axA.grid(axis="x", color=FAINT, lw=0.7); axA.set_axisbelow(True)
    for s in ("top", "right", "left"):
        axA.spines[s].set_visible(False)
    axA.set_title("a · pairwise, the primary evidence   (n=%d)" % len(p),
                  fontsize=9.8, color=MUTED, loc="left", pad=9)
    hs = [plt.Line2D([], [], marker=jmark[j], ls="", color=MUTED, alpha=0.65,
                     ms=6, label=j) for j in judges]
    hs.append(plt.Line2D([], [], marker="o", ls="", color=ACCENT, ms=9,
                         label="pooled"))
    axA.legend(handles=hs, loc="upper left", bbox_to_anchor=(0.0, -0.105),
               frameon=False, fontsize=8.2, ncol=4, handletextpad=0.4,
               columnspacing=1.1)

    # B — per-build mean9, reps joined
    for t in ["low", "mid", "high"]:
        s = tbl[tbl["tier"] == t]
        xj = TIER_ORDER[t] + np.linspace(-0.14, 0.14, len(s))
        axB.scatter(xj, s["mean9"], s=62, color=TIER_C[t], zorder=3,
                    ec=BG, lw=1.0)
        axB.plot([TIER_ORDER[t] - 0.3, TIER_ORDER[t] + 0.3],
                 [s["mean9"].mean()] * 2, color=INK, lw=2.0, zorder=4)
    # join the two haiku reps
    hk = tbl[tbl["model"] == "claude-haiku-4-5-20251001"].sort_values("mean9")
    if len(hk) == 2:
        s = tbl[tbl["tier"] == "low"].reset_index(drop=True)
        xs = {b: TIER_ORDER["low"] + v for b, v in
              zip(s["build_id"], np.linspace(-0.14, 0.14, len(s)))}
        a, b = hk.iloc[0], hk.iloc[1]
        axB.plot([xs[a.build_id], xs[b.build_id]], [a.mean9, b.mean9],
                 color=ACCENT, lw=1.6, zorder=2)
        axB.annotate("two reps of\nthe same model\n(Δ 2.05)",
                     xy=(xs[a.build_id], a.mean9), xytext=(0.42, 3.35),
                     fontsize=8.6, color=ACCENT, ha="left", va="center",
                     arrowprops=dict(arrowstyle="-", color=ACCENT, lw=1.0))
    axB.set_xticks([0, 1, 2]); axB.set_xticklabels(["low", "mid", "high"], fontsize=9.5)
    axB.set_xlim(-0.55, 2.55); axB.set_ylim(2.9, 6.1)
    axB.set_ylabel("mean of nine factors", fontsize=9.5)
    axB.grid(axis="y", color=FAINT, lw=0.7); axB.set_axisbelow(True)
    for s_ in ("top", "right"):
        axB.spines[s_].set_visible(False)
    axB.set_title("b · absolute scores, builds as units", fontsize=9.8,
                  color=MUTED, loc="left", pad=9)
    axB.text(0.5, 2.99, "Jonckheere–Terpstra  z = +2.44,  p = 0.015   (n = 15)",
             fontsize=8.4, color=MUTED, ha="left")

    # C — Krippendorff alpha
    alpha = {"trust": .195, "memorable": .271, "ease_of_use": .310,
             "intuitive": .322, "clarity": .359, "comfort": .379,
             "aesthetic_pleasure": .417, "interest": .447,
             "visual_hierarchy": .456}
    ks = sorted(alpha, key=alpha.get)
    axC.barh(range(len(ks)), [alpha[k] for k in ks], color="#c9c4bc",
             height=0.62)
    for i, k in enumerate(ks):
        if k == "visual_hierarchy":
            axC.barh([i], [alpha[k]], color=ACCENT, height=0.62)
        axC.text(alpha[k] + 0.012, i, f"{alpha[k]:.2f}", va="center",
                 fontsize=8.4, color=MUTED)
    axC.axvline(0.667, color=INK, lw=1.1, ls=(0, (4, 3)))
    axC.text(0.648, len(ks) - 0.62, "α = .667\nconventional floor",
             fontsize=8.2, color=INK, ha="right", va="bottom")
    axC.set_yticks(range(len(ks)))
    axC.set_yticklabels([FACT_T[k] for k in ks], fontsize=8.6, color=MUTED)
    axC.set_xlim(0, 0.85); axC.set_ylim(-0.6, len(ks) + 0.35)
    axC.set_xticks([0, 0.25, 0.5, 0.75])
    axC.tick_params(axis="x", labelsize=8.4)
    axC.set_xlabel("Krippendorff's α (ordinal)", fontsize=9.2)
    axC.grid(axis="x", color=FAINT, lw=0.7); axC.set_axisbelow(True)
    for s_ in ("top", "right", "left"):
        axC.spines[s_].set_visible(False)
    axC.set_title("c · inter-judge reliability", fontsize=9.8, color=MUTED,
                  loc="left", pad=9)

    fig.savefig(out, dpi=200)
    plt.close(fig)
    print("wrote", out)


# ------------------------------------------------------------------- figure 4
def fig4(root, tbl, out):
    """Quality against latency and list price — the handoff to the cost study."""
    g = tbl.groupby(["provider", "tier", "model"], as_index=False)["mean9"].mean()
    g["out_price"] = g["model"].map(lambda m: PRICE[m][1])
    g["in_price"] = g["model"].map(lambda m: PRICE[m][0])
    g["lat"] = g["model"].map(LAT)
    g["short"] = g["model"].map(lambda m: SHORT.get(m, m))

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12.4, 5.3))
    fig.subplots_adjust(left=0.07, right=0.985, top=0.755, bottom=0.135, wspace=0.20)

    fig.text(0.07, 0.945, "Figure 4", fontsize=13, fontweight="bold", ha="left")
    fig.text(0.07, 0.875,
             "Quality bought per second and per dollar. Nothing above ~5.6 is for "
             "sale at any price, and the cheapest tier is not the worst.",
             fontsize=10.5, color=MUTED, ha="left")

    def panel(ax, xcol, xlabel, title):
        for r in g.itertuples():
            ax.scatter([getattr(r, xcol)], [r.mean9], s=150,
                       color=PROV_C[r.provider],
                       alpha={"low": 0.42, "mid": 0.68, "high": 1.0}[r.tier],
                       ec=BG, lw=1.3, zorder=3)
        ax.set_xscale("log")
        ax.set_xlabel(xlabel, fontsize=9.5)
        ax.grid(color=FAINT, lw=0.7); ax.set_axisbelow(True)
        for s in ("top", "right"):
            ax.spines[s].set_visible(False)
        ax.set_title(title, fontsize=9.8, color=MUTED, loc="left", pad=9)
        ax.set_ylim(3.9, 6.05)

    panel(ax1, "lat", "mean seconds per turn   (log scale)",
          "a · against measured latency")
    panel(ax2, "out_price", "list price, $ per 1M output tokens   (log scale)",
          "b · against list price")
    ax1.set_ylabel("mean of nine factors", fontsize=9.5)

    # hand-placed labels: the two panels collide in different places
    OFF_A = {  # vs latency
        "gpt-5.5": (11, -12, "left"), "llama-3.3-70b": (0, 12, "center"),
        "gpt-oss-120b": (11, 3, "left"), "gpt-4.1": (-11, -4, "right"),
        "gpt-4o-mini": (2, -19, "center"), "claude-haiku-4-5": (11, -3, "left"),
        "deepseek-v4-pro": (0, 12, "center"), "claude-opus-5": (0, 12, "center"),
        "deepseek-v4-flash": (0, 12, "center"),
    }
    OFF_B = {  # vs list price
        "deepseek-v4-flash": (0, 12, "center"), "gpt-oss-120b": (-11, -4, "right"),
        "gpt-4o-mini": (0, -19, "center"), "llama-3.3-70b": (-12, 4, "right"),
        "deepseek-v4-pro": (10, 4, "left"), "claude-haiku-4-5": (0, -19, "center"),
        "gpt-4.1": (0, -19, "center"), "claude-opus-5": (-9, 11, "right"),
        "gpt-5.5": (4, -19, "center"),
    }
    for ax, xcol, off in ((ax1, "lat", OFF_A), (ax2, "out_price", OFF_B)):
        for r in g.itertuples():
            dx, dy, ha = off[r.short]
            ax.annotate(r.short, (getattr(r, xcol), r.mean9),
                        textcoords="offset points", xytext=(dx, dy),
                        ha=ha, fontsize=8.3, color=INK, zorder=5,
                        path_effects=[pe.withStroke(linewidth=2.6,
                                                    foreground=BG)])
        ax.set_yticks([4.0, 4.5, 5.0, 5.5, 6.0])

    # flag the implausible gpt-5.5 latency
    row = g[g["short"] == "gpt-5.5"].iloc[0]
    ax1.annotate("0.19 s/turn over 9/9 calls is not a\n"
                 "credible build turn — re-measure\nbefore this number is cited",
                 xy=(row["lat"], row["mean9"] + 0.03), xytext=(1.05, 5.88),
                 fontsize=8.2, color=ACCENT, ha="left", va="top", zorder=6,
                 arrowprops=dict(arrowstyle="-|>", color=ACCENT, lw=1.3,
                                 connectionstyle="arc3,rad=0.28"))

    # the one tier inversion in the study
    hi = g[g["short"] == "gpt-oss-120b"].iloc[0]
    lo = g[g["short"] == "llama-3.3-70b"].iloc[0]
    for ax, xcol in ((ax1, "lat"), (ax2, "out_price")):
        ax.plot([lo[xcol], hi[xcol]], [lo["mean9"], hi["mean9"]],
                color=PROV_C["groq"], lw=1.2, ls=(0, (3, 2)), zorder=2)
    ax2.annotate("groq's top tier scores\nbelow its own mid tier",
                 xy=((lo["out_price"] * hi["out_price"]) ** 0.5,
                     (lo["mean9"] + hi["mean9"]) / 2),
                 xytext=(2.1, 4.94), fontsize=8.2, color=PROV_C["groq"],
                 ha="left", va="center", zorder=6,
                 arrowprops=dict(arrowstyle="-", color=PROV_C["groq"], lw=1.0))

    hs = [plt.Line2D([], [], marker="o", ls="", color=PROV_C[p], ms=8, label=p)
          for p in ["groq", "openai", "anthropic", "deepseek"]]
    hs += [plt.Line2D([], [], marker="o", ls="", color=MUTED, ms=8,
                      alpha=a, label=f"{t} tier")
           for t, a in [("low", .42), ("mid", .68), ("high", 1.0)]]
    ax2.legend(handles=hs, loc="upper left", frameon=False, fontsize=8.2,
               ncol=2, handletextpad=0.4, labelspacing=0.32, columnspacing=1.0)

    fig.savefig(out, dpi=200)
    plt.close(fig)
    print("wrote", out)


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    outdir = os.path.join(root, "figures")
    os.makedirs(outdir, exist_ok=True)
    d = load(root)
    tbl = build_table(d)
    print(tbl[["build_id", "provider", "tier", "mean9"]].to_string(index=False))
    fig1(root, tbl, d, os.path.join(outdir, "fig_1_tier_ladder.png"))
    fig2(root, d, os.path.join(outdir, "fig_2_repair_loop.png"))
    fig3(root, tbl, d, os.path.join(outdir, "fig_3_judge_verdict.png"))
    fig4(root, tbl, os.path.join(outdir, "fig_4_quality_frontier.png"))


if __name__ == "__main__":
    main()
