# Appendix X, results: MLLM-judged UI quality of `oldowan` firmware output across provider tiers

Run 2026-08-16. Read `DEVIATIONS.md` before quoting anything here; the two
largest departures from the protocol are that the corpus has two generation
reps rather than three, and that Gate 1 is reported twice — once as the
protocol defines it, once as a user actually experiences it.

The resolved matrix is **10 model-tiers** across four providers, 19 first-pass
builds, 15 as-deployed builds, 90 captured frames.

| Provider | Low | Mid | High |
|---|---|---|---|
| anthropic | `claude-haiku-4-5-20251001` ×2 | *(no builds in corpus)* | `claude-opus-5` ×2 |
| openai | `gpt-4o-mini` ×2 | `gpt-4.1` ×2 | `gpt-5.5` ×2 |
| groq | `llama-3.1-8b-instant` ×2 | `llama-3.3-70b-versatile` ×2 | `openai/gpt-oss-120b` ×2 |
| deepseek | `deepseek-v4-flash` ×1 | *(none served)* | `deepseek-v4-pro` ×2 |

**The mid tier is the weak point of this run and every mid-tier number below
should be read with that in mind.** Two of the four providers contribute no mid
build at all: deepseek serves no mid model, and the corpus inherited from the
provider benchmark contains no `claude-sonnet-5` sketches. Of the four mid
builds that do exist, two survive to be judged. Any statement about the mid
tier here rests on n = 2, from two providers.

---

## X.1 Gate 1 — compile

**First pass: 5/19 (26%). As deployed, after the hidden repair loop: 15/19
(79%).**

| provider | tier | builds | first pass | as deployed |
|---|---|---:|---:|---:|
| anthropic | low | 2 | 0 | 2 |
| anthropic | high | 2 | 0 | 2 |
| deepseek | low | 1 | 0 | 1 |
| deepseek | high | 2 | 0 | 2 |
| groq | low | 2 | 0 | 0 |
| groq | mid | 2 | 0 | 1 |
| groq | high | 2 | 2 | 2 |
| openai | low | 2 | 1 | 2 |
| openai | mid | 2 | 0 | 1 |
| openai | high | 2 | 2 | 2 |

Pooled across providers, as-deployed compile is 71% at the low tier, 50% at the
mid, and **100% at the high tier**. The four builds that never compile at all
are both `llama-3.1-8b-instant` reps, one `llama-3.3-70b-versatile` rep, and one
`gpt-4.1` rep. All four exhaust all three repair passes still failing, which is
a stronger statement than failing once: a model that cannot fix its own sketch
given the compiler's own errors, three times over, is not merely slower to
converge.

Everything that does compile as-deployed does so on pass 1 or 2 — no build in
the corpus was rescued by the third attempt. Whatever `MAX_ATTEMPTS = 3` is
buying in production, on this brief it is not the third pass.

### The first-pass number is not a clean measure of the model

Three builds — `claude-opus-5` rep1 and rep2, and `deepseek-v4-pro` rep2 — are
rejected by `arduino-cli` and yet compile and run correctly in the Gate 2
harness. Their errors are all the same shape:

```
'BtnEvent' does not name a type
'RGB' was not declared in this scope
variable or field 'pollButton' declared void
```

This is the Arduino builder's own prototype generation. A `.ino` is not a C++
translation unit, so `arduino-cli` synthesises a forward declaration for every
function and inserts them at the top of the file — above the `struct` and
`enum` declarations those signatures mention. A sketch that defines a type and
then takes it as a parameter is valid C++ and is broken by the tool that builds
it. The Gate 2 harness generates the same prototypes but places them after the
last type declaration, and those three builds are fine.

All three are high-tier builds, which is not a coincidence: writing a `struct
Button` and a `void pollButton(Button&)` is a thing the more capable models do
and the weaker ones do not. **The first-pass compile rate therefore penalises
the high tier for a toolchain defect**, and the as-deployed number — where the
repair loop simply works around it — is both the fairer measure and the one a
user meets.

---

## X.2 Gates 2–3 — render and journey coverage

All 15 as-deployed builds compiled against the harness, ran to completion, and
produced all six frames: **90/90 frames captured, no absences.** Every build
called `setRotation`, so none of them drew portrait on a landscape panel.

Journey coverage, counted as *distinct* frames among the six:

| tier | builds | mean distinct (of 6) |
|---|---:|---:|
| low | 5 | 5.80 |
| mid | 2 | 6.00 |
| high | 8 | 5.88 |

**There is no tier effect in coverage** (Jonckheere–Terpstra z = 0.17,
p = 0.87). Every build in the corpus, weakest to strongest, distinguishes
essentially all six states of the journey. Two builds collapse one pair of
states — `gpt-oss-120b` rep2 and `gpt-4o-mini` rep1 each show 5 distinct frames
— and that is the whole of it.

This is worth stating plainly because the protocol expected coverage to be a
discriminator: "a build that only ever shows one screen has failed the brief
regardless of how that screen scores." No build in this corpus fails that way.
Once a sketch compiles, it answers the brief structurally. Whatever separates
the tiers is not *whether* the states are distinguished but *how they look*.

Two builds do draw screens with nothing on them — `claude-haiku-4-5` rep2 has
two flat-colour frames and `gpt-4o-mini` rep2 has one, three in all — which the
harness counts separately from an absent frame, since a judge shown a blank
panel is scoring nothing at all.

---

## X.3 What the screens actually show

Before any judge, the contact sheet (`results/contact_sheet.png`, and the
report page) separates the corpus visually, and the separation runs along the
device's own documented design language rather than along general graphic
taste. `device_profile.json` tells every model the same rules: one focal
element, two type sizes, no boxes or panel chrome, a bare near-black canvas
with a single off-white ink, and colour carrying meaning while touching as
little of the screen as possible.

- `claude-opus-5` (both reps) builds the ambient fluid fill that *is* the
  state: a liquid level rising through the panel, teal to amber to red as cost
  accumulates, with one large number and one letter-spaced label above it. It
  is the only build in the corpus that uses an animation to replace a number
  rather than to decorate one.
- `gpt-5.5` (both reps) renders a seven-segment number with a single thin
  accent rule beneath it that changes colour at the threshold. Restrained and
  legible.
- `gpt-oss-120b` rep1 floods the entire panel cyan, then orange, with magenta
  text on top. `llama-3.3-70b-versatile` rep2 floods it blue with magenta text.
  Both invert the rule they were given: colour covers everything and means
  nothing.
- `gpt-4o-mini` rep1 overflows the panel. At thirty minutes it renders
  `Cost:  $2` with the `9` wrapped onto the next line and overlapping
  `Duration: 29 min`. The number the whole device exists to show is unreadable
  at exactly the moment it matters.
- `deepseek-v4-flash` shows a green dot that grows and brightens. Its render
  path contains no text call of any kind — only `fillCircle` — so the cost is
  never displayed as a number anywhere in the journey. That is arguably a
  literal reading of turn 2 of the brief, *"silent, just the light"*, rather
  than a failure: the instruction was about suppressing the haptics, and the
  model took it as a description of the whole display. It is the one build in
  the corpus where the ambiguity of the user's own words, not the model's
  competence, best explains the screen — and it is also the cleanest
  demonstration that journey coverage says nothing about whether the brief was
  answered, since it distinguishes all six states while showing no cost at all.

---

## X.4 Judging

Three judges: `claude-sonnet-5`, `gpt-4.1`, and `qwen/qwen3.6-27b` on Groq —
one per model family, all vision-capable. Nine factors, 7-point scale, Luera
et al.'s Figure 7 prompt with the framing moved to a small embedded display.
Median of three repetitions per judge per frame.

Coverage is uneven and worth stating before the numbers. `claude-sonnet-5` and
`gpt-4.1` returned a complete nine-factor score on all 90 frames.
`qwen3.6-27b` returned one on **72 of 90**: it reasons in an unsuppressable
`<think>` block that frequently consumed its whole token budget before it
answered, and 18 frames have no usable rep at all. Its medians also rest on
fewer reps — 45 of its 90 frames are a median over one rep rather than three.
The open-weight judge is therefore the thinnest of the three, and its results
should be read as such.

### Reliability first

**Mean Krippendorff's α across the nine factors: 0.35.** By factor:

| factor | α |
|---|---:|
| visual hierarchy | 0.46 |
| interest | 0.45 |
| aesthetic pleasure | 0.42 |
| comfort | 0.38 |
| clarity | 0.36 |
| intuitive | 0.32 |
| ease of use | 0.31 |
| memorable | 0.27 |
| trust | 0.20 |

The protocol is explicit that low agreement invalidates the absolute scores,
and 0.35 is low by any conventional reading. **The Likert numbers below are
therefore descriptive only.** Three judges looking at the same 320×170 panel
do not agree on how trustworthy or memorable it is, and no amount of averaging
fixes that.

Note which factors hold up best: visual hierarchy, interest and aesthetic
pleasure — the ones answerable from the arrangement of the pixels. The ones
that collapse — trust at 0.20, memorable at 0.27 — are the ones that require
imagining a person's relationship with a device the judge has never held.
Luera et al. already flagged ease of use as near-random in their own pairwise
data; on this instrument it is one of the weaker three here too.

### Absolute scores, as description

Mean of the nine factors, builds as the unit of analysis (six frames of one
build are six views of one design, not six independent draws):

| tier | builds | mean of nine factors |
|---|---:|---:|
| low | 5 | 4.29 |
| mid | 2 | 4.80 |
| high | 8 | 5.23 |

Jonckheere–Terpstra ordinal trend: **z = +2.44, p = 0.015** (n = 15 builds).
Running the same test over the 252 frame-level rows returns z = +6.98,
p < 0.001; that number is reported here only to show how much treating nested
frames as independent inflates it.

Per factor, all three judges independently find a significant upward trend on
**visual hierarchy** (p = 0.008 / 0.009 / 0.031), **memorable**
(0.009 / 0.020 / 0.011) and **interest** (0.003 / 0.007 / 0.008); two of three
also find one on **aesthetic pleasure**. None of the three finds a trend on
**clarity**, **trust**, **intuitive** or **ease of use**. Clarity is the
instructive one: it scores 6.0–6.9 for every tier, because "the layout is
uncluttered" rewards an empty screen. `gpt-oss-120b`'s full-screen orange panel
with magenta text was scored 7/7 for clarity by `claude-sonnet-5` —
"only two lines of text on a plain background, extremely uncluttered" — while
scoring 3/7 for aesthetic pleasure in the same reply. The factor is measuring
sparseness, not design, and on a device whose whole idiom is sparseness it
cannot discriminate.

The single largest number in the table is not a tier effect at all.
`claude-haiku-4-5` rep1 scores **5.29** and rep2 scores **3.24** — the same
model, the same brief, two draws. That 2.05-point gap is larger than the whole
low-to-high tier difference of 0.94. Rep-to-rep variance within a model
exceeds the effect the study set out to measure, which is the strongest
argument in this run for the protocol's own "small n" caveat.

### Pairwise, the primary evidence

Every within-provider pair was judged twice, with the two builds swapped
between the UI-A and UI-B slots. Only agreement across both orderings counts;
a flip is recorded as a tie and never resolved. Position consistency was 84%
overall — real position bias, but modest — and the tie rate runs from 8%
(`qwen3.6-27b`) to 15% (`claude-sonnet-5`).

On cross-tier comparisons, the **higher tier won 77.8% of decided comparisons**
(n = 1842). All three judges agree, and they agree criterion by criterion:

| criterion | pooled | sonnet-5 | gpt-4.1 | qwen3.6 |
|---|---:|---:|---:|---:|
| visual hierarchy | **0.89** | 0.92 | 0.88 | 0.82 |
| memorable | 0.80 | 0.77 | 0.83 | 0.79 |
| interest | 0.79 | 0.76 | 0.82 | 0.78 |
| overall preference | 0.78 | 0.81 | 0.75 | 0.83 |
| ease of use | 0.77 | 0.78 | 0.75 | 0.83 |
| intuitive | 0.77 | 0.79 | 0.75 | 0.82 |
| clarity | 0.76 | 0.76 | 0.75 | 0.83 |
| trust | 0.75 | 0.72 | 0.76 | 0.83 |
| aesthetic pleasure | 0.75 | 0.71 | 0.77 | 0.88 |
| comfort | 0.70 | 0.64 | 0.72 | 0.93 |

`qwen3.6-27b` contributed 319 of a possible 1320 comparisons — the same
budget-exhaustion that thinned its absolute scores — so its column is noisier
than the other two and its high figures on comfort and aesthetic pleasure rest
on few decisions. The two full columns are the ones to weight.

Visual hierarchy is the discriminator, at 0.89 pooled and 0.92 for the judge
with the most data — which is exactly what the
contact sheet shows: the separation between tiers is one focal element against
a bare ground versus a flooded panel or an overflowing line of text. It is also
the factor with the highest inter-judge agreement, so the two analyses point the
same way.

---

## X.5 The two questions the study asked

**1. Does model tier predict the perceived quality of the on-device UI that
`oldowan` generates?**

Yes, on this instrument, and mostly through one channel.

- It does **not** predict whether the build answers the brief structurally.
  Coverage of the six journey states is flat across tiers (p = 0.87); every
  build that compiles distinguishes essentially all six.
- It **does** predict which of two builds a judge prefers: 77.8% of decided
  cross-tier comparisons go to the higher tier (n = 1842), and all three judges
  agree criterion by criterion.
- The channel is **visual hierarchy** — 0.89 pairwise, the strongest ordinal
  trend in the absolute scores, and the factor with the best inter-judge
  agreement. What separates the tiers is one focal element on a bare ground
  versus a flooded panel or a line of text that runs off the edge.
- The effect is **smaller than the within-model spread**. Two reps of
  `claude-haiku-4-5` differ by 2.05 points on the nine-factor mean; the
  low-to-high tier difference is 0.94.

**2. Is the tier difference large enough to justify the latency and cost
penalty of the higher tiers?**

On latency, mostly no — and the exceptions are not where the tier story would
put them.

| model | tier | mean of nine factors | mean s/turn | quality per second |
|---|---|---:|---:|---:|
| gpt-5.5 | high | 5.42 | 0.19 | 28.1 |
| llama-3.3-70b-versatile | mid | 4.96 | 0.76 | 6.5 |
| gpt-4.1 | mid | 4.63 | 1.17 | 3.9 |
| gpt-4o-mini | low | 4.34 | 1.31 | 3.3 |
| claude-haiku-4-5 | low | 4.27 | 2.43 | 1.8 |
| deepseek-v4-pro | high | 5.21 | 21.50 | 0.24 |
| claude-opus-5 | high | 5.59 | 49.43 | 0.11 |
| deepseek-v4-flash | low | 4.23 | 73.97 | 0.06 |

`claude-opus-5` produces the best screens in the corpus and takes roughly 260
times as long per turn as `gpt-5.5` to produce screens scoring 0.16 lower — a
difference well inside the within-model spread. On this evidence the case for
the top Anthropic tier is not the UI it draws.

Two cautions on that table. `gpt-5.5`'s 0.19 s mean comes straight from
`lith_provider_latency.xlsx` and is anomalous enough — an order of magnitude
faster than every other model, with a 0.012 s standard deviation across nine
calls — that it should be re-measured before anyone acts on it. And no
cost column is given: the benchmark's raw sheet records no token counts, and
list prices for the full resolved matrix are not written down in any existing
artefact, so quality-per-dollar is left undone rather than guessed.

The more useful finding for `oldowan` sits in Gate 1, not in the judging. The
repair loop moves compile success from 26% to 79% — an effect several times
larger than anything tier does to the screens, and it is a property of the
harness rather than of the model. Whatever tier `oldowan` runs on, the thing
most worth protecting is the loop that never shows a compile error to the
user.

---

## X.6 Limitations of this run

Everything in the protocol's own X.6 stands unchanged: no human validation, an
instrument borrowed from full-size web screens and applied to a 320×170
glanceable panel, static frames that discard the animation which is the best
builds' whole idea, and one brief. Added by this run:

- **Inter-judge agreement is low (α = 0.35).** By the protocol's own rule this
  invalidates the absolute scores; they are reported as description and the
  pairwise result carries the argument.
- **The open-weight judge is thin.** `qwen3.6-27b` produced usable absolute
  scores on 72 of 90 frames, half of them from a single rep, and 319 of a
  possible 1320 pairwise comparisons. It agrees with the other two on
  direction, but it is not an equal third vote.
- **The mid tier has two builds, from two providers.** deepseek serves no mid
  model, the corpus contains no `claude-sonnet-5` sketches at all, and of the
  four mid builds that exist only one `llama-3.3-70b-versatile` and one
  `gpt-4.1` survived to be judged. Every mid-tier number here rests on n = 2,
  and the low/mid/high split of judged builds is 5 / 2 / 8. A trend test over
  three groups where the middle one has two members is being carried by its
  endpoints; the low-versus-high contrast is the part to trust.
- **Two reps, not three**, and rep-to-rep variance turned out to exceed the
  tier effect — so this is the limitation that matters most for anyone wanting
  to act on the result.

The `gpt-oss-120b` row is missing from the table above because its own
quality-per-second (4.00, from 1.17 s/turn) sits mid-pack and adds nothing to
the argument; it is in `results/analysis.txt` with everything else.
