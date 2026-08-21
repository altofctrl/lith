#!/usr/bin/env python3
"""Compact companion to Figure 1, sized for a Word document.

Figure 1 proper is 15 builds x 6 states at the panel's native 320x170, which is
2735x3052 px -- unreadable once Word scales it into a column. This draws the
same journey for one representative build per tier, at a physical width that
drops straight into a 16 cm text column and stays legible.

Representative = the build whose nine-factor mean is the median within its tier,
so this is not a best-of-tier showcase.

    python harness/make_figure_1b.py .
"""
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.image as mpimg
import matplotlib.pyplot as plt

from make_figures import (BG, FAINT, INK, MUTED, SHORT, STATE_T, STATES,
                          TIER_C, build_table, load)

WIDTH_IN = 6.30      # ~16 cm, a single Word column
LABEL_W = 1.38       # left gutter for the model name
RIGHT_W = 0.14
GAP = 0.035
ROW_GAP = 0.16
TOP = 0.62
BOT = 0.30


def pick_representative(tbl):
    """Median build within each tier, so the row is typical rather than best."""
    out = []
    for tier in ("low", "mid", "high"):
        rows = tbl[tbl.tier == tier].sort_values("mean9")
        if rows.empty:
            continue
        out.append(rows.iloc[len(rows) // 2])
    return out


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    frames = os.path.join(root, "frames", "deployed")
    out = os.path.join(root, "figures", "fig_1b_journey_compact.png")

    tbl = build_table(load(root))
    rows = pick_representative(tbl)

    cw = (WIDTH_IN - LABEL_W - RIGHT_W - 5 * GAP) / 6
    ch = cw * 170 / 320
    n = len(rows)
    H = TOP + n * ch + (n - 1) * ROW_GAP + BOT
    W = WIDTH_IN

    fig = plt.figure(figsize=(W, H))

    def ax_at(x, y, w, h):
        return fig.add_axes([x / W, 1 - (y + h) / H, w / W, h / H])

    fig.text(0.10 / W, 1 - 0.20 / H, "Figure 1b", ha="left", va="bottom",
             fontsize=9.5, fontweight="bold", color=INK)
    fig.text(0.80 / W, 1 - 0.205 / H,
             "one representative build per tier, same six-state journey",
             ha="left", va="bottom", fontsize=8.0, color=MUTED)

    for c, lab in enumerate(STATE_T):
        x = LABEL_W + c * (cw + GAP)
        fig.text((x + cw / 2) / W, 1 - (TOP - 0.10) / H, lab, ha="center",
                 va="bottom", fontsize=6.4, color=MUTED)

    y = TOP
    for r in rows:
        for c, st in enumerate(STATES):
            ax = ax_at(LABEL_W + c * (cw + GAP), y, cw, ch)
            p = os.path.join(frames, f"{r.build_id}__{st}.png")
            if os.path.exists(p):
                ax.imshow(mpimg.imread(p), interpolation="antialiased",
                          aspect="auto")
            else:
                ax.set_facecolor("#efece7")
            ax.set_xticks([]); ax.set_yticks([])
            for s in ax.spines.values():
                s.set_color(FAINT); s.set_linewidth(0.5)

        # tier swatch + model, in the left gutter
        sw = ax_at(0.02, y + ch / 2 - 0.035, 0.07, 0.07)
        sw.set_facecolor(TIER_C[r.tier]); sw.set_xticks([]); sw.set_yticks([])
        for s in sw.spines.values():
            s.set_visible(False)

        fig.text(0.15 / W, 1 - (y + ch / 2 - 0.058) / H,
                 f"{r.tier} tier", ha="left", va="center",
                 fontsize=7.0, color=INK, fontweight="bold")
        fig.text(0.15 / W, 1 - (y + ch / 2 + 0.058) / H,
                 f"{SHORT.get(r.model, r.model)}   {r.mean9:.2f}/7",
                 ha="left", va="center", fontsize=6.0, color=MUTED)
        y += ch + ROW_GAP

    fig.text(0.10 / W, 0.12 / H,
             "Panels are the device's own pixels. Full 15-build matrix at "
             "native resolution: Figure 1.",
             ha="left", va="bottom", fontsize=6.2, color=MUTED)

    fig.savefig(out, dpi=300, facecolor=BG)
    plt.close(fig)
    print(f"wrote {out}  ({W:.2f}x{H:.2f} in at 300 dpi)")


if __name__ == "__main__":
    main()
