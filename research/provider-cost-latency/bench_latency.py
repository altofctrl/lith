"""Re-run the lith_provider_latency benchmark for one or more providers.

Replays a real 3-turn oldowan session (session 0ab8e5ec045142f9a74c3bbd5cda0cac,
"meeting cost meter") verbatim: at each turn we send the accumulated real
transcript up to and including that user message, time the provider's
response, and append the assistant's actual reply to the running history
before moving to the next turn. 3 reps x 3 turns, matching the existing
raw sheet in lith_provider_latency.xlsx.

Usage: python3 bench_latency.py <provider_name> [model_override] [reps]
Provider must be a key in providers.json["providers"] (used for endpoint/format/
headers/timeout). model_override lets you run a different model than the
provider's configured default, e.g. to benchmark several tiers on one provider.
Writes results as CSV rows (provider,model,rep,turn,latency_s,ok) to stdout.
"""

import sys
import time

import oldowan

REAL_USER_TURNS = [
    "a meeting cost meter i can start when a meeting starts",
    "silent, just the light",
    "what next?",
]


def load_named_provider(name):
    cfg = oldowan._load_json("providers.json")
    prov = dict(cfg["providers"][name])
    prov["_name"] = name
    prov["_timeout"] = cfg.get("request_timeout_s", 150)
    secrets = oldowan.load_secrets()
    import re

    def expand(value):
        return re.sub(
            r"\$\{([A-Za-z0-9_]+)\}",
            lambda m: str(secrets.get(m.group(1)) or __import__("os").environ.get(m.group(1), "")),
            value,
        )

    prov["headers"] = {k: expand(v) for k, v in prov.get("headers", {}).items()}
    return prov


def _is_reasoning_openai_model(model):
    # gpt-5.x / o1 / o3 / o4 reject non-default temperature and use
    # max_completion_tokens instead of max_tokens. oldowan.py's own
    # _call_openai targets gpt-4.1-era models only, so we shim around it
    # here rather than touching production code.
    return model.startswith(("gpt-5", "o1", "o3", "o4"))


def _call_openai_compat(prov, messages):
    import requests

    reasoning = _is_reasoning_openai_model(prov["model"])
    body = {
        "model": prov["model"],
        ("max_completion_tokens" if reasoning else "max_tokens"): prov.get("max_tokens", 8000),
        "messages": [{"role": "system", "content": oldowan.system_prompt()}]
        + [{"role": m["role"], "content": m["content"]} for m in messages],
    }
    if "temperature" in prov and not reasoning:
        body["temperature"] = prov["temperature"]
    # deepseek-v4-* defaults to reasoning_effort "high", which on this
    # system prompt can burn the entire max_tokens budget on invisible
    # reasoning tokens and return empty content (finish_reason "length").
    # Capping effort keeps a real answer inside a normal token budget.
    if prov["model"].startswith("deepseek-v4"):
        body["thinking"] = {"type": "enabled", "reasoning_effort": "low"}
    r = requests.post(prov["endpoint"], headers=prov["headers"], json=body,
                       timeout=prov["_timeout"])
    r.raise_for_status()
    data = r.json()
    return data["choices"][0]["message"]["content"]


def call(prov, messages):
    fmt = prov["format"]
    if fmt == "anthropic":
        return oldowan._call_anthropic(prov, messages)
    if fmt == "openai":
        return _call_openai_compat(prov, messages)
    raise ValueError(f"unsupported format {fmt}")


def main():
    if len(sys.argv) < 2:
        print("usage: bench_latency.py <provider_name> [model_override] [reps]", file=sys.stderr)
        sys.exit(1)
    name = sys.argv[1]
    rest = sys.argv[2:]
    model_override = None
    if rest and not rest[0].isdigit():
        model_override = rest.pop(0)
    reps = int(rest[0]) if rest else 3

    prov = load_named_provider(name)
    if model_override:
        prov["model"] = model_override
    print("provider,model,rep,turn,latency_s,ok")
    for rep in range(1, reps + 1):
        history = []
        for turn, user_text in enumerate(REAL_USER_TURNS, start=1):
            history.append({"role": "user", "content": user_text})
            t0 = time.perf_counter()
            ok = True
            try:
                reply = call(prov, history)
            except Exception as e:
                ok = False
                body = getattr(getattr(e, "response", None), "text", "")
                reply = f"[error] {e} {body}"
            dt = time.perf_counter() - t0
            history.append({"role": "assistant", "content": reply})
            print(f"{name},{prov['model']},{rep},{turn},{dt:.6f},{ok}")
            sys.stderr.write(f"rep {rep} turn {turn}: {dt:.3f}s ok={ok}"
                              + (f" -- {reply[:200]}" if not ok else "") + "\n")


if __name__ == "__main__":
    main()
