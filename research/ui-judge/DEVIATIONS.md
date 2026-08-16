# Deviations from the protocol as written

Every point where this run departs from `oldowan-ui-judge-microstudy.md`, with
the reason. Listed here rather than buried in the results so that anything read
off the numbers can be discounted appropriately.

## Design and corpus

**Two reps per model, not three.** The protocol specifies 3 generation reps per
model for 36 builds. This run reuses the corpus already generated for the
provider benchmark (`bench_all/code`), which has 2 reps, giving 19 builds across
11 models. Regenerating a third rep is 11 models x ~5 turns of live API calls
and was not affordable inside the session's time budget. The consequence is that
"small n" in X.6 is smaller still: 2 reps is enough to see a tier-scale
difference and not enough to say anything about a single model.

**The matrix resolved differently from the table in X.2.** X.2 names
`gpt-4o-mini` / `gpt-4.1-mini` / `gpt-4.1` for openai and "largest available"
for the other high tiers. The corpus was generated against what the providers
were actually serving, and the resolved matrix is:

| Provider | Low | Mid | High |
|---|---|---|---|
| Provider | Low | Mid | High |
|---|---|---|---|
| anthropic | `claude-haiku-4-5-20251001` ×2 | *(no builds)* | `claude-opus-5` ×2 |
| openai | `gpt-4o-mini` ×2 | `gpt-4.1` ×2 | `gpt-5.5` ×2 |
| groq | `llama-3.1-8b-instant` ×2 | `llama-3.3-70b-versatile` ×2 | `openai/gpt-oss-120b` ×2 |
| deepseek | `deepseek-v4-flash` ×1 | *(none served)* | `deepseek-v4-pro` ×2 |

Two gaps, both in the mid tier. deepseek has no mid model at all:
`deepseek-chat` and `deepseek-reasoner` resolve into the same v4 family, so the
provider offers two tiers, not three. And the inherited corpus contains **no
`claude-sonnet-5` sketches**, so anthropic's mid tier is missing entirely —
which is the one gap that could have been closed by regenerating, and was not,
for the time reason above. `deepseek-v4-flash` has one rep rather than two.

The matrix is therefore **10 model-tiers, not 12**, and badly unbalanced across
tiers: 7 low builds, 4 mid, 8 high, of which 5 / 2 / 8 survive to be judged.
The mid tier is the one to distrust. X.2's own note that "aliases drift" is why
the table and the run disagree on names; the resolved names are what the
analysis uses.

**One provider-block parameter had to be dropped per model.** `providers.json`
carries one config block per provider, tuned for the model that block normally
runs; the anthropic block sets `effort: medium`, which `claude-sonnet-5`
supports and `claude-haiku-4-5` rejects outright with a 400. A tier sweep across
one provider therefore cannot hold *all* sampling parameters at production
defaults the way X.2 asks. `effort` is dropped for the haiku tier only and left
in place everywhere it is accepted.

## Gate 1

**Gate 1 is reported twice.** The protocol measures compile success on the
model's first sketch. That is not what a workshop participant experiences:
`builder.py` never shows a compile error to the user, it returns the errors to
Oldowan as a hidden `[internal]` message and recompiles, up to `MAX_ATTEMPTS = 3`
passes. Since X.2 states the object of study is "`oldowan` as deployed", this
run reports both **first-pass** compile (the protocol's measure) and
**as-deployed** compile (after the production repair loop). The as-deployed
corpus is what Gates 2-3 render and what the judges score, because it is the
firmware a user would actually be holding.

**The repair loop runs on a reconstructed history.** Production carries the
model's own intermediate `ask` turns into the repair. The bench corpus kept only
the final sketch, so the reconstructed history is the three real user turns plus
the model's own build envelope, then the verbatim `[internal]` repair prompt
from `builder.py`. The repair reasons over the errors and its own code, which is
the load-bearing part, but it does not see the questions it asked on the way
there.

**The earlier `bench_build.py` CSVs are not used.** They are internally
inconsistent with the `.ino` files sitting beside them (rows reporting no build
for models that have sketch files on disk), having been written across several
partial runs with different nudge settings, and `bench_build.py` mishandles the
anthropic reply shape: `oldowan._call_anthropic` returns
`{text, stop_reason, usage}` while the openai-compatible path returns a bare
string, and the bench passes the dict straight to `parse_envelope`. Gate 1 here
is re-run from scratch over the corpus so it is one consistent table.

## Gate 2 (render)

**A native SDL-shim build, not Wokwi.** X.3 allows either. The harness
(`harness/sim/`) compiles each sketch against the real LovyanGFX with a
RAM-backed `Panel_FrameBufferBase` in place of the ST7789-on-SPI, so the pixels
come from the library the device runs: same glyph rasteriser, same RGB565
quantisation, same rotation maths, reading the sketch's own `offset_rotation`
out of its own panel config. Only the bus and the backlight are inert, because
neither appears in a frame.

**`Panel_Device::init` is skipped.** It drives the reset line, the backlight and
then `_bus->init()`; with no bus that last call is a dereference of null. What
it would otherwise do is hardware bring-up with no bearing on a pixel.

**Prototypes are generated, as arduino-cli does, but placed differently.** A
`.ino` is not a translation unit; the Arduino builder generates a forward
prototype for every sketch function before compiling. The harness does the same,
or sketches would fail here for a reason that has nothing to do with the model.
It places the block after the last top-level type declaration rather than at the
top of the file. arduino-cli's top-of-file placement is what produces the
`'Button' was not declared in this scope` failures visible in Gate 1 — the
prototypes land above the structs they mention. Gate 1 has already measured
that; Gate 2 asks what the UI looks like, and reproducing the placement bug here
would only stop screens from being seen.

**`-fpermissive`.** These sketches target the Arduino toolchain and a few lean
on its laxer conversions. Gate 1 is where strictness is the question.

## Gate 3 (journey)

**Time is stepped, not simulated continuously.** `millis()` is virtual and the
harness advances it in 1000 ms steps between capture points, tightening to 100
ms and then 20 ms in the last half-second before each capture. One second is the
coarsest safe stride: a meter that accrues on a `now - last >= 1000` tick
advances once per loop, so a longer step would under-count the money on screen —
the number being judged. A build that counts iterations rather than reading the
clock is still mis-driven by this, and the device profile does tell models to
use `millis()`.

**One scripted input sequence for every build.** SW1 is tapped to start and
tapped again to stop. Builds that chose SW2, or a hold, or an encoder turn as
their start control are not accommodated — by design, per X.3: the journey comes
from the brief, not from each build's own idea of its controls. A build whose
frames never change is recorded as covering one state, and that is a finding
about the build meeting the brief, not a harness failure. `rotation_set` and
`init_called` are recorded separately so a build that never rotated the panel is
visible as such rather than silently corrected.

**Absence is measured as frame-identity, not as a claim about intent.** Coverage
counts *distinct* frames among the six. Two journey states that render
identically are counted once.

## X.4 (judging)

**Three reps per judge, not ten.** The protocol takes the median of 10
repetitions per judge per frame to suppress sampling noise. This run uses 3.
Ten would have been ~3x the calls for a median that moves little; the cost is a
noisier median, which matters most where judges are near-indifferent.

**The third judge is Qwen, not Llama.** X.4 requires three MLLMs from different
families. Groq no longer hosts a Llama vision model, so the open-weight slot is
`qwen/qwen3.6-27b`. The other two are `claude-sonnet-5` and `gpt-4.1`. Three
distinct families is the property the protocol asks for, and it holds.

**Judge thinking is set to low effort.** `claude-sonnet-5` has extended thinking
on by default. Luera et al.'s judges answer directly, and a reasoning trace is
not part of the instrument.

**The prompts are the paper's, with one substitution and one added format
line.** The nine Table 1 factors, the 7-point scale, the Figure 7 wording and
the Task 2 criterion/result/reason structure are as published. The framing moves
from "an average user brought in to do human testing" to a user encountering a
small embedded device display, which X.4 specifies. A single sentence pinning
the response layout is appended so three model families can be parsed the same
way; it adds no content to the instrument.

**The open-weight judge's data is thinner than the other two.**
`claude-sonnet-5` and `gpt-4.1` returned a complete nine-factor score on all 90
frames. `qwen3.6-27b` did so on 72 of 90, and 45 of those rest on a median over
one rep rather than three: it reasons in a `<think>` block that cannot be turned
off and that often consumed the whole token budget before it answered, even at a
6,000-token ceiling. The first attempt at its pass also lost 211 of 270 calls to
groq rate limits before backoff was added. This is reported rather than
smoothed, because a missing score and a low score are not the same thing.

**Two parsing faults were found and fixed mid-run, and the affected judge was
re-run.** `gpt-4.1` answers the pairwise task with the criterion as the tag name
(`<ease of use>[UI-A]</ease of use><result>UI-A</result>`) rather than inside a
`<criterion>` element, which a `<criterion>`-anchored pattern misses entirely;
that silently dropped 18% of its comparisons on the first pass. The parser now
anchors on `<result>` and attributes each to the criterion named last before it,
and `gpt-4.1`'s whole pairwise task was re-run, recovering 1320/1320. The
pairwise raw log now also keeps the reply text, which it did not on the first
pass — without it a parse failure cannot be told from a judge that declined to
answer.

**Pairwise runs both orderings.** Not in the protocol, added here. An MLLM shown
two images has a position bias, and without the swap a bias toward the first
slot is indistinguishable from a real preference — it would surface as a tier
effect that is really an artefact of listing order. Each pair is judged twice
with the builds swapped; only agreement across both orders counts as a
preference, and a flip is recorded as a tie rather than resolved.

## One thing the protocol gets wrong about its own source

X.4 and X.6 justify treating the pairwise task as primary on the grounds that
Luera et al.'s "pairwise agreement is strong when the two options genuinely
differ, reaching roughly 90 percent for the strongest judges at large score
gaps." The paper's *overall* pairwise accuracy against human preference is
52.96–59.98% (Claude 59.98%, GPT-4o 59.60%); the ~90% figure applies only to
the subset of comparisons where the two UIs were separated by a large human
score gap. That is a much narrower claim than the protocol's phrasing carries.

It does not change the design — pairwise is still the better-behaved of the two
tasks, and this run's own inter-judge α of 0.35 independently says the absolute
scores cannot carry the argument — but the pairwise result here should be read
against ~53–60% baseline alignment, not against 90%. The protocol's numbers for
the absolute task (72–77% within one Likert point, 35–38% exact) match the
paper.

## Still not done

- **No human validation**, as X.4 specifies by design. Everything in X.6 about
  the borrowed instrument stands unchanged, and is the study's main weakness.
- **Static frames only.** The motion extension floated in X.6 is not attempted.
- **One brief.** As specified.
- **Quality per second is computed; quality per dollar is not.** The latency
  join uses the per-model means already in `lith_provider_latency.xlsx`. No
  cost column is given: the benchmark's raw sheet records no token counts, and
  list prices for the full resolved eleven-model matrix are not written down in
  any existing artefact. That join needs a live price check first, so it is
  left undone rather than guessed at.
- **`gpt-5.5`'s latency figure is not trustworthy and is flagged in the
  results.** The source sheet gives 0.193 s mean with a 0.012 s standard
  deviation over nine calls — an order of magnitude faster than everything else
  and implausibly consistent. It is carried through unaltered because
  re-measuring it is a separate job, but nothing should be concluded from the
  quality-per-second column for that model until it is.
