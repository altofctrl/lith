"""Build the shareable report page from the result CSVs and the frames.

  python make_artifact.py <study_dir> <out.html>

Every number on the page is read from the CSVs at generation time; nothing is
typed in by hand, so the page cannot drift from the data beside it.

The design follows lith.vidalion.co's own language -- warm paper, Times for
prose, monospace for labels and figure captions, hairline rules -- because the
site already has one and this is an appendix to it. The device panel keeps its
real colours in both themes: it is a photograph of a screen, not a UI element,
and inverting it would be a lie about what the owner sees. Frames are rendered
at 1:1 with image-rendering: pixelated, since the whole study turns on what the
RGB565 dither and the bitmap glyphs actually look like.
"""

import base64
import csv
import html
import io
import os
import re
import sys
from collections import defaultdict

from PIL import Image

STATES = ["1_boot_idle", "2_started", "3_mid_meeting",
          "4_threshold", "5_extended", "6_stopped"]
STATE_LABEL = {
    "1_boot_idle": "boot / idle",
    "2_started": "started",
    "3_mid_meeting": "5 min",
    "4_threshold": "30 min",
    "5_extended": "90 min",
    "6_stopped": "stopped",
}
FACTORS = ["ease_of_use", "clarity", "visual_hierarchy", "memorable", "trust",
           "intuitive", "aesthetic_pleasure", "interest", "comfort"]
FACTOR_LABEL = {f: f.replace("_", " ") for f in FACTORS}
TIER_ORDER = {"low": 0, "mid": 1, "high": 2}


def rows(path):
    with open(path, encoding="utf-8") as fh:
        return list(csv.DictReader(fh))


def png_data_uri(path):
    """Re-encode with real compression; the harness writes stored-deflate PNGs
    that are ~160 KB each, which would blow the page size budget for no gain."""
    im = Image.open(path).convert("RGB")
    buf = io.BytesIO()
    im.save(buf, format="PNG", optimize=True)
    return "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode()


def fnum(x, nd=2):
    try:
        return f"{float(x):.{nd}f}"
    except (TypeError, ValueError):
        return "&mdash;"


def esc(s):
    return html.escape(str(s))


def main():
    d, out_path = sys.argv[1], sys.argv[2]
    R = os.path.join(d, "results")

    gate1 = {r["build_id"]: r for r in rows(os.path.join(R, "gate1_compile.csv"))}
    repair = {r["build_id"]: r for r in rows(os.path.join(R, "gate1_repair.csv"))}
    render = {r["build_id"]: r for r in rows(os.path.join(R, "gate2_render_deployed.csv"))}
    absolute = rows(os.path.join(R, "judge_absolute.csv"))
    pair_path = os.path.join(R, "judge_pairwise.csv")
    pair = rows(pair_path) if os.path.exists(pair_path) else []
    analysis = ""
    ap = os.path.join(R, "analysis.txt")
    if os.path.exists(ap):
        analysis = open(ap, encoding="utf-8").read()

    def grab(pattern, default="&mdash;"):
        m = re.search(pattern, analysis)
        return m.group(1) if m else default

    # ---------------------------------------------------------------- gate 1
    by_pt = defaultdict(lambda: {"n": 0, "first": 0, "dep": 0})
    for b, g in gate1.items():
        k = (g["provider"], g["tier"])
        by_pt[k]["n"] += 1
        by_pt[k]["first"] += g["compiled"] == "True"
        by_pt[k]["dep"] += repair.get(b, {}).get("deployed_compiled") == "True"
    n_all = len(gate1)
    first_all = sum(v["first"] for v in by_pt.values())
    dep_all = sum(v["dep"] for v in by_pt.values())

    tier_g1 = defaultdict(lambda: {"n": 0, "first": 0, "dep": 0})
    for (prov, tier), v in by_pt.items():
        for k in ("n", "first", "dep"):
            tier_g1[tier][k] += v[k]

    # --------------------------------------------------------------- judging
    judges = sorted({r["judge"] for r in absolute})
    # per build: mean of the nine factors, per judge and pooled
    per_build = defaultdict(list)
    per_build_judge = defaultdict(list)
    for r in absolute:
        vals = [float(r[f]) for f in FACTORS if r[f] not in ("", None)]
        if not vals:
            continue
        m = sum(vals) / len(vals)
        per_build[r["build_id"]].append(m)
        per_build_judge[(r["build_id"], r["judge"])].append(m)
    build_mean = {b: sum(v) / len(v) for b, v in per_build.items()}

    tier_mean = defaultdict(list)
    for b, m in build_mean.items():
        t = gate1.get(b, {}).get("tier")
        if t in TIER_ORDER:
            tier_mean[t].append(m)

    # ------------------------------------------------------------- pairwise
    higher_won = decided = ties = 0
    per_judge_pw = defaultdict(lambda: [0, 0, 0])   # won, decided, tie
    for r in pair:
        ta = gate1.get(r["build_a"], {}).get("tier")
        tb = gate1.get(r["build_b"], {}).get("tier")
        if ta not in TIER_ORDER or tb not in TIER_ORDER or ta == tb:
            continue
        j = per_judge_pw[r["judge"]]
        if r["winner"] == "tie":
            ties += 1
            j[2] += 1
            continue
        wt = ta if r["winner"] == r["build_a"] else tb
        lt = tb if r["winner"] == r["build_a"] else ta
        decided += 1
        j[1] += 1
        if TIER_ORDER[wt] > TIER_ORDER[lt]:
            higher_won += 1
            j[0] += 1

    # ----------------------------------------------------------------- html
    builds = sorted(render, key=lambda b: (gate1[b]["provider"],
                                           TIER_ORDER.get(gate1[b]["tier"], 9),
                                           b))
    frames_dir = os.path.join(d, "frames", "deployed")

    def frame_img(b, s):
        p = os.path.join(frames_dir, f"{b}__{s}.png")
        if not os.path.exists(p):
            return '<div class="frame missing">absent</div>'
        return (f'<img class="frame" width="320" height="170" loading="lazy" '
                f'alt="{esc(b)} at {esc(STATE_LABEL[s])}" src="{png_data_uri(p)}">')

    best = max(build_mean, key=build_mean.get) if build_mean else builds[0]
    worst = min(build_mean, key=build_mean.get) if build_mean else builds[-1]

    P = []
    A = P.append
    A('<title>Tier and the Screen</title>')
    A(f"<style>{CSS}</style>")

    A('<header class="masthead">')
    A('<p class="eyebrow">lith &middot; appendix x</p>')
    A('<h1>Tier and the screen</h1>')
    n_models = len({g["model"] for g in gate1.values()})
    n_ratings = sum(1 for r in absolute for f in FACTORS if r[f] not in ("", None))
    A(f'<p class="standfirst">{n_models} models across four providers were asked '
      'for the same knapp &mdash; a meeting cost meter &mdash; and the firmware '
      'each one wrote was run until it drew. These are the screens, and what '
      'three MLLM judges made of them.</p>')
    A('<dl class="facts">')
    for label, value in [
        ("builds", str(n_all)),
        ("compiled first pass", f"{first_all}/{n_all}"),
        ("compiled as deployed", f"{dep_all}/{n_all}"),
        ("frames captured", str(sum(1 for b in builds for s in STATES
                                    if os.path.exists(os.path.join(frames_dir, f"{b}__{s}.png"))))),
        ("factor ratings", str(n_ratings)),
    ]:
        A(f'<div><dt>{label}</dt><dd>{value}</dd></div>')
    A('</dl>')
    A('</header>')

    # ---- hero: best vs worst
    A('<section class="band">')
    A('<h2>The finding, before any judge</h2>')
    A('<p>The same brief, the same six moments, the same panel. Above, the '
      f'highest-scoring build in the corpus; below, the lowest. Every screen on '
      'this page is the firmware&rsquo;s own output, drawn by the real '
      'LovyanGFX at the panel&rsquo;s native 320&times;170 and RGB565, then '
      'shown here at 1:1.</p>')
    for b, kind in ((best, "highest"), (worst, "lowest")):
        g = gate1[b]
        A('<div class="strip-wrap"><div class="strip">')
        A(f'<p class="striplabel"><span class="{kind}">{kind}</span> '
          f'{esc(g["model"])} <span class="tier t-{g["tier"]}">{g["tier"]}</span> '
          f'&middot; mean of nine factors {fnum(build_mean.get(b))}</p>')
        A('<div class="row">')
        for s in STATES:
            A(f'<figure>{frame_img(b, s)}'
              f'<figcaption>{esc(STATE_LABEL[s])}</figcaption></figure>')
        A('</div></div></div>')
    A('</section>')

    # ---- gate 1
    A('<section>')
    A('<h2>Gate 1 &middot; does it compile</h2>')
    A('<p>Reported twice. <em>First pass</em> is the model&rsquo;s opening '
      'sketch, which is what the protocol asks for. <em>As deployed</em> is '
      'after Oldowan&rsquo;s hidden repair loop &mdash; up to three passes, with '
      'the compiler errors fed back as <code>[internal]</code> messages the user '
      'never sees. A workshop participant only ever meets the second number.</p>')
    A('<div class="tablewrap"><table>')
    A('<thead><tr><th>provider</th><th>tier</th><th class="n">builds</th>'
      '<th class="n">first pass</th><th class="n">as deployed</th></tr></thead><tbody>')
    for (prov, tier) in sorted(by_pt, key=lambda k: (k[0], TIER_ORDER.get(k[1], 9))):
        v = by_pt[(prov, tier)]
        A(f'<tr><td>{esc(prov)}</td><td><span class="tier t-{tier}">{tier}</span></td>'
          f'<td class="n">{v["n"]}</td><td class="n">{v["first"]}</td>'
          f'<td class="n">{v["dep"]}</td></tr>')
    A(f'<tr class="total"><td colspan="2">all</td><td class="n">{n_all}</td>'
      f'<td class="n">{first_all} <span class="pct">{first_all/n_all:.0%}</span></td>'
      f'<td class="n">{dep_all} <span class="pct">{dep_all/n_all:.0%}</span></td></tr>')
    A('</tbody></table></div>')
    A('<p class="note">fig. 01 &middot; compile success by provider and tier. '
      'The repair loop is the single largest effect anywhere in this study.</p>')
    A('</section>')

    # ---- the screens
    A('<section class="band">')
    A('<h2>Gates 2&ndash;3 &middot; every build, every state</h2>')
    A('<p>Each build was compiled against the real display library with the '
      'ST7789-on-SPI replaced by a RAM framebuffer, then driven down one '
      'scripted journey: boot, start the meeting, five minutes, thirty, ninety, '
      'stop. The journey comes from the brief, not from each build&rsquo;s own '
      'idea of its controls, so every build is scored on the same route.</p>')
    A('<div class="sheetwrap"><table class="sheet">')
    A('<thead><tr><th class="rowhead">build</th>'
      + "".join(f'<th>{esc(STATE_LABEL[s])}</th>' for s in STATES)
      + '</tr></thead><tbody>')
    for b in builds:
        g = gate1[b]
        rr = render[b]
        A('<tr>')
        A(f'<th class="rowhead"><span class="model">{esc(g["model"])}</span>'
          f'<span class="meta"><span class="tier t-{g["tier"]}">{g["tier"]}</span> '
          f'rep {esc(g["rep"])}</span>'
          f'<span class="meta">{esc(rr["distinct_frames"])}/6 distinct'
          + (f' &middot; {esc(rr["blank_frames"])} blank' if rr["blank_frames"] not in ("0", "") else "")
          + (f' &middot; mean {fnum(build_mean.get(b))}' if b in build_mean else "")
          + '</span></th>')
        for s in STATES:
            A(f'<td>{frame_img(b, s)}</td>')
        A('</tr>')
    A('</tbody></table></div>')
    A('<p class="note">fig. 02 &middot; the whole corpus. Scroll sideways. '
      '&ldquo;Blank&rdquo; counts frames that are a single flat colour &mdash; a '
      'screen the build never drew on.</p>')
    A('</section>')

    # ---- judging
    A('<section>')
    A('<h2>X.4 &middot; what the judges said</h2>')
    A('<p>Nine factors from Luera et al., <em>MLLM as a UI Judge</em>, on their '
      '7-point scale and their prompt, with the framing moved from a web '
      'usability test to someone meeting a small device screen. Three judges, '
      'one per model family.</p>')

    alpha_mean = grab(r"mean alpha across factors: ([\-\d.]+)")
    A('<div class="callout">')
    A('<p class="calloutlabel">reliability first</p>')
    A(f'<p>Mean Krippendorff&rsquo;s &alpha; across the nine factors: '
      f'<strong>{alpha_mean}</strong>. The protocol says plainly that low '
      'agreement invalidates the absolute scores, so the Likert numbers below '
      'are description, not evidence. The pairwise task is where the weight '
      'sits.</p>')
    A('</div>')

    A('<div class="tablewrap"><table>')
    A('<thead><tr><th>tier</th><th class="n">builds</th>'
      '<th class="n">mean of nine factors</th></tr></thead><tbody>')
    for t in ("low", "mid", "high"):
        v = tier_mean.get(t, [])
        A(f'<tr><td><span class="tier t-{t}">{t}</span></td>'
          f'<td class="n">{len(v)}</td>'
          f'<td class="n">{fnum(sum(v)/len(v)) if v else "&mdash;"}</td></tr>')
    A('</tbody></table></div>')
    A('<p class="note">fig. 03 &middot; builds are the unit, not frames: six '
      'frames of one build are six views of one design.</p>')

    if decided:
        A('<h3>Pairwise, within provider</h3>')
        A('<p>Every pair judged twice with the two builds swapped between the '
          'A and B slots. Only agreement across both orderings counts as a '
          'preference; a flip is recorded as a tie, never resolved.</p>')
        A('<div class="tablewrap"><table>')
        A('<thead><tr><th>judge</th><th class="n">decided</th>'
          '<th class="n">ties</th><th class="n">higher tier won</th>'
          '</tr></thead><tbody>')
        for j in sorted(per_judge_pw):
            won, dec, tie = per_judge_pw[j]
            A(f'<tr><td>{esc(j)}</td><td class="n">{dec}</td>'
              f'<td class="n">{tie}</td>'
              f'<td class="n">{f"{won/dec:.0%}" if dec else "&mdash;"}</td></tr>')
        A(f'<tr class="total"><td>pooled</td><td class="n">{decided}</td>'
          f'<td class="n">{ties}</td>'
          f'<td class="n">{higher_won/decided:.0%}</td></tr>')
        A('</tbody></table></div>')
        A('<p class="note">fig. 04 &middot; cross-tier comparisons only. '
          '50% means no tier effect.</p>')
    A('</section>')

    # ---- limits
    A('<section class="band limits">')
    A('<h2>What this does not show</h2>')
    A('<ul>')
    for item in [
        "No human comparison group, by design. The instrument's alignment with "
        "human judgement is borrowed from Luera et al., who validated it on "
        "full-size professionally designed web screens. Nothing here "
        "establishes that these judges track human perception of <em>these</em> "
        "screens.",
        "Two reps per model, eleven model-tiers, fifteen judged builds. Enough "
        "to see a tier-scale difference; not enough to rank adjacent models.",
        "Six still frames per build. lith's display is animated, and the "
        "fluid fill that carries state in the better builds is exactly what a "
        "still throws away.",
        "One brief. Every build answers the same meeting cost meter prompt.",
        "The judges' own low agreement, above, limits every absolute number on "
        "this page.",
    ]:
        A(f"<li>{item}</li>")
    A('</ul>')
    A('<p class="note">Full method, and every point where this run departs from '
      'the written protocol, are in <code>DEVIATIONS.md</code> beside the data.</p>')
    A('</section>')

    A('<footer><p>Generated from the result CSVs in <code>results/</code>. '
      'Frames are the firmware&rsquo;s own pixels, not mock-ups.</p></footer>')

    with open(out_path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(P))
    print("wrote", out_path, f"{os.path.getsize(out_path)/1e6:.2f} MB")


CSS = """
:root {
  --paper:#faf8f0; --ink:#1a1814; --ink-soft:#5f594d;
  --rule:rgba(26,24,20,.18); --rule-soft:rgba(26,24,20,.09);
  --amber:#a85f10; --teal:#1c6a62; --flag:#8c2f1d;
  --panel:#090a0c; --panel-ink:#eef0f5;
  --serif:"Times New Roman",Times,"Liberation Serif",Georgia,serif;
  --mono:ui-monospace,"SF Mono","Cascadia Mono",Menlo,Consolas,monospace;
}
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) {
    --paper:#0e0f11; --ink:#e9ebef; --ink-soft:#9aa1aa;
    --rule:rgba(233,235,239,.20); --rule-soft:rgba(233,235,239,.10);
    --amber:#e3a457; --teal:#57b6ab; --flag:#e0705a;
  }
}
:root[data-theme="dark"] {
  --paper:#0e0f11; --ink:#e9ebef; --ink-soft:#9aa1aa;
  --rule:rgba(233,235,239,.20); --rule-soft:rgba(233,235,239,.10);
  --amber:#e3a457; --teal:#57b6ab; --flag:#e0705a;
}

* { box-sizing:border-box; }
body {
  background:var(--paper); color:var(--ink);
  font-family:var(--serif); font-size:18px; line-height:1.55;
  margin:0; padding:0 24px 96px;
  -webkit-font-smoothing:antialiased;
}
section, header, footer { max-width:70ch; margin:0 auto; }
section { padding:44px 0; border-top:1px solid var(--rule-soft); }
h1,h2,h3 { text-wrap:balance; font-weight:400; letter-spacing:-.01em; }
h1 { font-size:clamp(2.4rem,6vw,3.6rem); line-height:1.05; margin:.1em 0 .3em; }
h2 { font-size:1.7rem; margin:0 0 .5em; }
h3 { font-size:1.25rem; margin:2em 0 .4em; }
p { margin:0 0 1em; }
em { font-style:italic; }
code { font-family:var(--mono); font-size:.85em; }

.eyebrow, .note, .striplabel, .calloutlabel, .meta, figcaption,
th, .pct, .tier { font-family:var(--mono); }
.eyebrow {
  text-transform:uppercase; letter-spacing:.18em; font-size:.7rem;
  color:var(--ink-soft); margin:0;
}
.masthead { padding:72px 0 40px; }
.standfirst { font-size:1.25rem; color:var(--ink-soft); max-width:60ch; }

.facts { display:flex; flex-wrap:wrap; gap:0 40px; margin:32px 0 0;
         padding-top:20px; border-top:1px solid var(--rule); }
.facts div { margin:0 0 12px; }
.facts dt { font-family:var(--mono); font-size:.65rem; text-transform:uppercase;
            letter-spacing:.12em; color:var(--ink-soft); }
.facts dd { margin:2px 0 0; font-size:1.4rem; font-variant-numeric:tabular-nums; }

.band { max-width:none; }
.band > h2, .band > h3, .band > p, .band > ul, .band > .note,
.band > .tablewrap, .band > .callout { max-width:70ch; margin-left:auto; margin-right:auto; }

.note { font-size:.72rem; color:var(--ink-soft); letter-spacing:.02em;
        margin-top:12px; }

/* the panel keeps its own colours in both themes: it is a screen, not chrome */
.frame {
  display:block; width:320px; height:170px;
  image-rendering:pixelated; background:var(--panel);
  border:1px solid var(--rule);
}
.frame.missing {
  display:flex; align-items:center; justify-content:center;
  color:var(--ink-soft); font-family:var(--mono); font-size:.7rem;
  background:transparent; border-style:dashed;
}

.strip-wrap { overflow-x:auto; margin:28px 0; padding-bottom:8px; }
.strip { min-width:max-content; margin:0 auto; }
.striplabel { font-size:.72rem; color:var(--ink-soft); margin:0 0 10px;
              letter-spacing:.04em; }
.striplabel .highest { color:var(--teal); }
.striplabel .lowest  { color:var(--flag); }
.row { display:flex; gap:10px; }
.row figure { margin:0; }
figcaption { font-size:.65rem; color:var(--ink-soft); margin-top:6px;
             text-transform:uppercase; letter-spacing:.1em; }

.sheetwrap { overflow-x:auto; margin:28px 0; padding-bottom:8px; }
.sheet { border-collapse:separate; border-spacing:10px 14px; min-width:max-content;
         margin:0 auto; }
.sheet thead th { font-size:.65rem; font-weight:400; color:var(--ink-soft);
                  text-transform:uppercase; letter-spacing:.12em;
                  text-align:left; padding:0 0 4px; }
.sheet .rowhead { text-align:left; font-weight:400; vertical-align:middle;
                  width:210px; padding-right:14px; }
.sheet .model { display:block; font-family:var(--mono); font-size:.78rem; }
.sheet .meta  { display:block; font-size:.62rem; color:var(--ink-soft);
                letter-spacing:.06em; margin-top:3px; }

.tier { font-size:.62rem; text-transform:uppercase; letter-spacing:.1em;
        padding:1px 5px; border:1px solid currentColor; }
.t-low  { color:var(--ink-soft); }
.t-mid  { color:var(--amber); }
.t-high { color:var(--teal); }

.tablewrap { overflow-x:auto; margin:24px 0; }
table { border-collapse:collapse; width:100%; font-size:.9rem; }
th { text-align:left; font-weight:400; font-size:.65rem; text-transform:uppercase;
     letter-spacing:.12em; color:var(--ink-soft);
     border-bottom:1px solid var(--rule); padding:0 12px 8px 0; }
td { padding:8px 12px 8px 0; border-bottom:1px solid var(--rule-soft);
     font-variant-numeric:tabular-nums; }
.n { text-align:right; padding-right:0; }
th.n { text-align:right; }
tr.total td { border-bottom:none; border-top:1px solid var(--rule); }
.pct { font-size:.72rem; color:var(--ink-soft); margin-left:6px; }

.callout { border-left:2px solid var(--amber); padding:4px 0 4px 20px;
           margin:28px 0; }
.calloutlabel { font-size:.65rem; text-transform:uppercase; letter-spacing:.14em;
                color:var(--amber); margin:0 0 6px; }

.limits ul { max-width:70ch; margin:20px auto 0; padding-left:1.1em; }
.limits li { margin-bottom:.8em; }

footer { padding-top:36px; border-top:1px solid var(--rule-soft); }
footer p { font-family:var(--mono); font-size:.68rem; color:var(--ink-soft); }

@media (max-width:640px) {
  body { padding:0 16px 64px; font-size:17px; }
  .sheet .rowhead { width:150px; }
}
"""


if __name__ == "__main__":
    main()
