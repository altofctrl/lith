# What the co-design agent actually produces

*Drop-in report section. Sits after the introduction of the co-design agent and
before the cost study. Figures 1–4 are in `figures/`; renumber to match the
report's scheme. ~1,850 words.*

---

Everything said so far describes Oldowan as an *interaction*: a question-driven
agent that elicits a brief from someone who cannot write firmware, and hands
back something they can flash. What it has not described is the *artefact*. And
the artefact has an unusual property that makes it unusually easy to evaluate:
because a knapp draws directly to the device's 320×170 colour panel, the output
of a generation run is not source code that happens to have a UI — it *is* a
user interface. It can be looked at, and it can be judged as one.

Figure 1 is that output. Fifteen builds, produced by nine models across four
providers from a single three-turn brief — *"a meeting cost meter I can start
when a meeting starts"*, *"silent, just the light"*, *"what next?"* — each
driven down the same six-state journey and photographed at every state. These
are not mock-ups or model self-descriptions. They are the device's own pixels:
the same LovyanGFX glyph rasteriser, the same RGB565 quantisation, the same
rotation arithmetic read out of each sketch's own panel configuration, with only
the SPI bus and the backlight replaced by inert stubs. Nothing in the rendering
path differs between rows, so everything that differs between rows is the
firmware.

**Figure 1** — *The tier ladder in pixels.* Every deployed build (rows, grouped
by provider tier and sorted by judged quality within tier) at every journey
state (columns), at native resolution. Callouts: **(i)** a `claude-haiku-4-5`
build that compiles and runs but draws nothing for two of the six states;
**(ii)** `gpt-4o-mini` overflowing its own text so that the cost figure `$29`
wraps and collides with the duration line beneath it; **(iii)** `claude-opus-5`
carrying a label, a value, a threshold colour and a waveform in the same 320×170
without crowding. Right-hand column: mean of the nine judged factors.

Read across a row and you can see whether a build understood that a meeting cost
meter has *states*. Read down a column and the spread is stark. At the bottom of
the low tier is a build that renders a single coloured rectangle. Near the top of
the high tier is one that changes typographic weight, background colour and
chart fill as the number crosses a threshold. Both compiled. Both ran. Both
would have been flashed to a real device and handed to a real person.

## The instrument, and where it comes from

Scoring these frames needs an instrument, and the one used here is adapted from
Luera et al.'s *MLLM as a UI Judge* (arXiv:2510.08783), an Adobe Research
benchmark of multimodal LLMs as predictors of human perception of interfaces.
Their contribution is a nine-factor battery — ease of use, clarity, visual
hierarchy, memorability, trust, intuitiveness, aesthetic pleasure, interest and
comfort — validated against 9,296 human Likert responses over thirty
interfaces, with GPT-4o, Claude 3.5 Sonnet and Llama-3.2-11B-Vision as judges.
Their headline results are the reason the study is designed the way it is: the
judges land within one Likert point of the human mean 72–77% of the time, but
*exact* score prediction is weak (35–38%), while pairwise preference agreement
rises to roughly 90% for the strongest judges once two options genuinely differ.

That paper is the UI-specific descendant of a now-standard evaluation method.
LLM-as-a-judge was established by Zheng et al. (*Judging LLM-as-a-Judge with
MT-Bench and Chatbot Arena*, NeurIPS 2023 Datasets & Benchmarks,
arXiv:2306.05685), which showed a strong model scoring open-ended text
responses could reach the ~80% agreement with human preferences that humans
reach with each other, and which named the biases that come with it — position,
verbosity, and self-enhancement. Luera et al. carry that method across the
modality gap into rendered interfaces; this study carries it one gap further,
onto a 320×170 embedded panel. The chain of borrowed validation is real and it
gets thinner at every link, which is why the limitations below are stated as
plainly as they are.

Nine factors were scored on a 7-point scale by three judges from three model
families (`claude-sonnet-5`, `gpt-4.1`, `qwen3.6-27b`), each scored three times
with the median taken to suppress sampling noise, followed by Luera et al.'s
second task: within-provider pairwise preference, every pair shown in both A/B
orderings so that a judge that flips when the options are swapped is counted as
having no preference rather than being silently resolved.

## Gate 1: the repair loop is the largest effect in the study

Before any frame could be drawn, each sketch had to compile. This is where the
most consequential finding sits, and it is not about model tier at all.

**Figure 2** — *First-pass versus as-deployed compile success.* Hollow markers:
the sketch as the model first wrote it. Filled markers: the same sketch after
`builder.py`'s repair passes. Right panel: of the fourteen builds that failed
first pass, how many were rescued, and on which pass.

Only 5 of 19 sketches — 26% — compiled as written. After the repair loop, 15 of
19 did: 79%. The production system never shows a compile error to the user; it
returns the toolchain's errors to Oldowan as a hidden `[internal]` message and
recompiles. That invisible loop is worth 53 percentage points — and it lifts
the high tier from 50% to 100%, and the low tier from 14% to 71%, so it is
worth *more* to the cheap models than to the expensive ones. A workshop
participant's odds of ending the session holding working firmware are set far
more by whether that loop exists than by which model is behind it.

The right panel adds a detail worth acting on: every single rescue landed on
pass two. `MAX_ATTEMPTS = 3` bought nothing on this brief. Errors a compiler can
describe and a model can fix get fixed immediately; the four that never
compiled were not converging, and a third attempt only spends money and time.

This is gap 4.2 of the review in concrete form. The reliability gain is real and
it is entirely architectural — it comes from closing a loop with the toolchain,
exactly as EmbedGenius and EmbedAgent predict. But it closes *around* the user.
The person who most needs to understand why their first sketch was wrong is the
one person the mechanism is designed to keep from ever seeing it.

## What the judges separated, and how far to trust them

Journey coverage — did the build distinguish all six states — turned out not to
discriminate at all (Jonckheere–Terpstra J = 34.5, z = 0.17, p = 0.87). Almost
every build that compiles distinguishes almost every state. What separates them
is how the states are *drawn*.

**Figure 3** — *(a)* Within-provider pairwise win rate for the higher tier, by
factor, per judge and pooled (n = 1,842 position-consistent cross-tier
comparisons). *(b)* Per-build nine-factor means by tier, builds as the unit of
analysis. *(c)* Inter-judge reliability by factor.

Pooled over judges, the higher tier wins 77.8% of cross-tier pairwise
comparisons. But the factor-level breakdown in panel (a) is the actual result:
**visual hierarchy at 89%** stands clear of everything else, and all three
judges independently rank it first (0.92, 0.88, 0.82). Comfort trails at 70%.
The ordering is not arbitrary — the factors the judges separate confidently are
the ones a still frame can carry, and the ones they separate weakly are the ones
that need a user, a task and elapsed time. Ease of use scores 77% here, which is
precisely the factor Luera et al. found near-random against human raters; it is
reported and not interpreted.

Panel (b) is the honest counterweight. The tier trend across builds is real and
ordered (z = +2.44, p = 0.015, n = 15), but the low-to-high gap is 0.94 points,
while **two runs of the identical model with identical settings differ by
2.05** — the orange pair, both `claude-haiku-4-5`. Rep-to-rep variance is more
than twice the tier effect. Choosing a better tier shifts the distribution;
it does not buy a particular outcome. There is a tier inversion too:
`gpt-oss-120b`, groq's top tier, scores *below* `llama-3.3-70b`, its mid tier.

Panel (c) sets the ceiling on all of it. Krippendorff's α across the three
judges averages 0.351 and peaks at 0.456, far below the 0.667 conventionally
required to treat ordinal ratings as reliable. The absolute Likert numbers in
this study are descriptive only. The pairwise task, which does not require the
judges to agree on a *scale*, carries the argument — which is exactly the
division of labour Luera et al. recommend, arrived at here independently.

## What quality costs

Joining these scores to the provider benchmark gives the picture the cost study
takes up in detail.

**Figure 4** — *Nine-factor mean against measured latency (a) and list price
(b).* Marker colour is provider, opacity is tier. Both axes log.

Three things carry forward. First, the ceiling is low: nothing in the corpus
exceeds 5.62 on a 7-point scale, and the entire judged range is 3.24 to 5.62.
There is no tier you can buy that produces a great interface, only tiers that
make an adequate one more likely. Second, price and quality are close to
decoupled at the top: `deepseek-v4-pro` scores 5.21 against `claude-opus-5`'s
5.59 — a gap of 0.37, well inside rep-to-rep noise — while costing $0.87 per
million output tokens against $25, a 29× multiple. Third, the `gpt-5.5` latency
of 0.193 s/turn, stable across 9/9 calls, is not a credible build-turn time and
is flagged in the figure; it should be re-measured before it is used in any
quality-per-second argument.

## Limitations

Two reps per model, not three, and an inherited corpus: the resolved matrix is
ten model-tiers rather than twelve, unbalanced at 7 low / 4 mid / 8 high, with
`claude-sonnet-5` missing entirely and deepseek serving no true mid tier. The
mid tier is the one to distrust. All builds answer one brief, so nothing
generalises to other things a participant might ask for. Six still frames
discard motion, which is a substantial part of a glanceable display. And most
importantly, **the instrument's human alignment is borrowed rather than
earned**: Luera et al. validated these factors on full-size, professionally
designed web screens, and no human ratings of *these* screens were collected.
This section reports how MLLM judges rank the output of different provider
tiers. It does not establish that lith's users would rank them the same way, and
the review's own recommendation — validate the judge against a subset of human
ratings — remains outstanding.

What the section does establish is narrower and still useful: the co-design
agent's visible output is measurable, it varies far more within a model than
between tiers, its reliability comes from a repair loop the user never sees, and
the quality it buys does not scale with what it costs. That last point is where
the cost study begins.
