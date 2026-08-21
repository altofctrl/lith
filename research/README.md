# research

Two studies, both aimed at the same question from different sides: **can a
language model be trusted to write the firmware a beginner asked for?** One
looks at the quality of what comes out, the other at what it costs to get it.

Both run against Oldowan, the co-design agent in [`../website/agent/`](../website/agent/).

---

## ui-judge

19 firmware builds, generated from one brief by 11 models across 5 providers,
compiled, rendered on a simulator that runs the sketches as the device would,
and then judged.

### the repair loop matters more than the model

This is the finding that changed how the agent is built. Straight out of the
model, only 5 of 19 builds compiled. After `builder.py`'s repair passes, 15 of
19 did.

![First-pass versus as-deployed compile rate, and which pass rescued each build](ui-judge/figures/fig_2_repair_loop.png)

Every rescued build was fixed on **pass 2**. `MAX_ATTEMPTS = 3` bought nothing
on this brief, which means the third attempt is cost with no return.

### tier buys hierarchy, not coverage

Every build that compiles distinguishes more or less all six states of the
journey, so journey coverage does not separate the tiers at all (p = 0.87).
What separates them is whether the screen is laid out well.

![15 builds by 6 journey states, sorted by judged quality within tier](ui-judge/figures/fig_1_tier_ladder.png)

On the pairwise task the higher tier wins 0.90 of the time. But rep-to-rep
variance beats the tier effect: two `claude-haiku-4-5` runs of the same prompt
differ by 2.05 on the nine-factor mean, where the whole low-to-high tier gap is
0.94.

![Pairwise win rate by factor, per-build scores by tier, and inter-judge agreement](ui-judge/figures/fig_3_judge_verdict.png)

Inter-judge agreement is weak (Krippendorff's alpha 0.35), so the absolute
Likert scores are descriptive only. The pairwise comparison is what carries the
argument.

![Nine-factor mean against measured latency and against list price](ui-judge/figures/fig_4_quality_frontier.png)

### where to look

| | |
|---|---|
| [`RESULTS.md`](ui-judge/RESULTS.md) | The full write-up of the run. |
| [`SECTION_codesign_agent.md`](ui-judge/SECTION_codesign_agent.md) | The ~1,800-word report section this became. |
| [`DEVIATIONS.md`](ui-judge/DEVIATIONS.md) | Where the run departed from the written protocol. |
| [`results/`](ui-judge/results/) | Every CSV and raw judge response. |
| [`code/`, `code_deployed/`](ui-judge/code/) | The sketches as generated, and as they ended up after repair. |
| [`harness/`](ui-judge/harness/) | The gates, the judges, the simulator and `make_figures.py`. |

Run 2026-08-16; written up 2026-08-20.

---

## provider-cost-latency

Wall-clock latency and list price across every provider Oldowan can be pointed
at, replayed through one real 3-turn conversation taken verbatim from a session
rather than a synthetic prompt.

![Mean latency per turn by model, grouped by provider](provider-cost-latency/lith_provider_latency.png)

Groq is the latency floor by a wide margin, which is the inference hardware
rather than a small-model effect. The reasoning-heavier models are both slower
and far noisier, and for an agent that streams its answer to someone waiting,
the spread matters as much as the mean.

![Input and output list price across the 11-model matrix](provider-cost-latency/lith_provider_cost.png)

Run 2026-08-10.

### read the spreadsheet, not the prose

Three things anyone using this data needs to know first:

- **`lith_provider_latency.md` is stale against `lith_provider_latency.xlsx`.**
  The prose describes an 8-model run; the sheet, saved 37 minutes later, is an
  11-model run that disagrees hard (sonnet-5 at 39.087 s mean, not 1.78 s
  stdev). Trust the sheet.
- `bench_build.py` and `bench_latency.py` hand `oldowan._call_anthropic`'s
  return value straight to `parse_envelope`, but the anthropic path returns a
  dict where the openai path returns a bare string. The anthropic rows in the
  older CSVs are wrong because of it.
- The `gpt-5.5` figure (0.193 s mean, 0.012 s stdev) is not plausible and needs
  re-measuring before it is cited anywhere.

One more trap for anyone re-running a tier sweep: `providers.json` sets
`effort: medium`, and `claude-haiku-4-5` returns a 400 on that field. It has to
be dropped for that model specifically.
