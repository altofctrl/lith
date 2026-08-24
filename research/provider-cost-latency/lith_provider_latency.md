# lith / Oldowan provider benchmark

Latency and list-price comparison across the LLM providers `oldowan.py` (the
lith firmware co-design agent) can run on. Backing data: `lith_provider_latency.xlsx`
(`raw` sheet = every timed call, `latency` sheet = per-model summary).

## Method

Each provider/model is run through the same real 3-turn `oldowan` conversation —
session `0ab8e5ec045142f9a74c3bbd5cda0cac`, the "meeting cost meter" build — taken
verbatim from `lith-backend/sessions/`:

1. *"a meeting cost meter i can start when a meeting starts"*
2. *"silent, just the light"*
3. *"what next?"*

Each turn is timed as a real multi-turn call: the accumulated conversation (system
prompt + prior turns) is sent, the response is timed, then the model's actual
reply is appended before the next turn — not three independent one-shot calls.
This is repeated for **3 reps × 3 turns = 9 timed calls per model**. `mean (s)`
and `stdev (s)` in the summary sheet are across all 9. The replay script is
`lith-backend/bench_latency.py`.

## lith_provider_latency.png — latency by model

Mean wall-clock time per turn, lowest to highest within each provider group.
Error bars are ±1 stdev across the 9 calls.

- **groq** (`llama-3.1-8b-instant`, `llama-3.3-70b-versatile`) is the clear
  latency floor — both models return in well under half a second. This is
  Groq's LPU inference hardware, not a smaller-model effect alone.
- **openai** (`gpt-4.1`, `gpt-4o-mini`) sits in the 1–1.5s range, with
  `gpt-4.1` slightly *faster* than the smaller `gpt-4o-mini` in this run —
  within noise given the stdevs, not a reliable ranking.
- **anthropic** (`claude-haiku-4-5`, `claude-sonnet-5`) is the slowest tier
  here, and also the noisiest: `claude-sonnet-5`'s stdev (1.78s) means real
  turns swing from ~4s to ~8s. This is expected — Sonnet is doing more
  reasoning per turn, not a broken result.
- **deepseek** (`deepseek-chat`, currently served as `deepseek-v4-flash` — see
  note below) lands mid-pack at 4.15s mean, but with the *widest* spread of
  any model tested (stdev 5.89s): two of its three turns were fast
  (1.9–2.6s) and one spiked to 7.9s. One rep is not enough to call this
  reliable — it's flagged as a single-sample outlier, not a verdict on
  DeepSeek's typical latency.

**DeepSeek model note:** `deepseek-chat` is DeepSeek's stable alias — it
currently routes to `deepseek-v4-flash` server-side (confirmed via the API
response's `model` field at benchmark time). If DeepSeek repoints the alias
again, re-run `bench_latency.py deepseek` to refresh.

## lith_provider_cost.png — cost by model

List API pricing per 1M tokens, **not** measured spend from this benchmark
(the raw sheet doesn't record token counts, so this is priced separately from
each provider's public pricing page, checked live while building this chart).
Solid bar = input price, faded bar = output price.

- groq is 15–40x cheaper than everything else on this chart at both the
  input and output rate — consistent with its role here as the fast/cheap
  tier for latency-sensitive turns (e.g. guard-model classification calls
  in `oldowan.py`, which use `llama-3.1-8b-instant`).
- `claude-sonnet-5` ($3 / $15 per 1M) is the most expensive model
  benchmarked, matching its position as the slowest-but-most-capable option.
- deepseek's list price ($0.14 / $0.28) sits just above groq — cheap, but not
  cheap enough to offset its latency variance for a latency-sensitive device
  like lith unless that variance turns out to be a fluke.

## Reading latency and cost together

Groq is unambiguous for anything latency-critical (the guard/adversarial
check in `oldowan.py` already uses `llama-3.1-8b-instant` for this reason).
For the main conversational agent, the real trade is `gpt-4.1` (fast,
mid-price) against `claude-sonnet-5` (slow, high-price, currently the
active provider in `providers.json`) — deepseek is a candidate worth
re-benchmarking with more reps before treating it as a serious third option,
given how much its one bad turn skews its current numbers.
