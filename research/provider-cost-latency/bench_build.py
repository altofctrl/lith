"""Benchmark the actual code-creation step, not just chat latency.

Replays the same real 3-turn oldowan conversation as bench_latency.py, but for
each rep also parses the model's envelope and, the first time a turn returns a
`build`, writes the generated .ino to a scratch sketch dir and compiles it with
arduino-cli (the same compile step builder.py runs before a real flash) --
measuring whether the model's own generated firmware actually compiles, and how
long that compile takes.

Usage: python3 bench_build.py <provider_name> [model_override] [reps]
Writes results as CSV rows to stdout:
  provider,model,rep,turn_of_first_build,envelope_valid,has_build,code_bytes,
  static_check_ok,compiled,compile_s,error_summary
"""

import json
import os
import re
import shutil
import sys
import tempfile
import time

import builder
import oldowan

CODE_DIR = os.environ.get(
    "BENCH_CODE_DIR",
    "/tmp/claude-1001/-home-aaron/43daeaa9-6d8c-4497-816e-1d88fef4f8e7/scratchpad/bench_all/code",
)

REAL_USER_TURNS = [
    "a meeting cost meter i can start when a meeting starts",
    "silent, just the light",
    "what next?",
]

# Live models each ask their own follow-up questions, so the fixed answers
# above don't always land -- most models don't reach a build within 3 turns.
# These bounded, uniform nudges are appended (same wording for every model)
# only if turn 3 didn't produce a build, so every model reliably reaches a
# build for code-quality/compile testing. turn_of_first_build > 3 is itself
# a signal: it means the model needed nudging rather than converging on its
# own within the same conversation every other model saw.
FALLBACK_NUDGES = [
    "please build it now, using your best judgement for anything not yet specified.",
    "just build it -- pick sensible defaults for anything unresolved.",
]

FQBN = None  # loaded from device_profile.json below


def load_named_provider(name):
    cfg = oldowan._load_json("providers.json")
    prov = dict(cfg["providers"][name])
    prov["_name"] = name
    prov["_timeout"] = cfg.get("request_timeout_s", 150)
    secrets = oldowan.load_secrets()
    import os

    def expand(value):
        return re.sub(
            r"\$\{([A-Za-z0-9_]+)\}",
            lambda m: str(secrets.get(m.group(1)) or os.environ.get(m.group(1), "")),
            value,
        )

    prov["headers"] = {k: expand(v) for k, v in prov.get("headers", {}).items()}
    return prov


def _is_reasoning_openai_model(model):
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


def csv_escape(s, limit=300):
    s = (s or "").replace("\n", " ").replace("\r", " ").replace(",", ";")
    return s[:limit]


def main():
    global FQBN
    if len(sys.argv) < 2:
        print("usage: bench_build.py <provider_name> [model_override] [reps]", file=sys.stderr)
        sys.exit(1)
    name = sys.argv[1]
    rest = sys.argv[2:]
    model_override = None
    if rest and not rest[0].isdigit():
        model_override = rest.pop(0)
    reps = int(rest[0]) if rest else 2

    profile = oldowan.device_profile()
    FQBN = profile["fqbn"]

    prov = load_named_provider(name)
    if model_override:
        prov["model"] = model_override

    print("provider,model,rep,turn_of_first_build,envelope_valid,has_build,"
          "code_bytes,static_check_ok,compiled,compile_s,error_summary")

    for rep in range(1, reps + 1):
        history = []
        first_build_turn = None
        first_build_code = None
        envelope_valid_last = False
        has_build_last = False

        all_turns = list(REAL_USER_TURNS)
        turn = 0
        while turn < len(all_turns):
            turn += 1
            user_text = all_turns[turn - 1]
            history.append({"role": "user", "content": user_text})
            try:
                reply = call(prov, history)
            except Exception as e:
                reply = f"[error] {e}"
            history.append({"role": "assistant", "content": reply})

            try:
                envelope = oldowan.parse_envelope(reply)
                envelope_valid_last = True
                has_build_last = bool(envelope and envelope.get("build"))
            except Exception:
                envelope = None
                envelope_valid_last = False
                has_build_last = False

            if first_build_turn is None and has_build_last:
                first_build_turn = turn
                first_build_code = envelope["build"].get("code", "")

            if turn == len(all_turns) and first_build_turn is None and len(all_turns) == len(REAL_USER_TURNS):
                all_turns.extend(FALLBACK_NUDGES)

        code_bytes = len(first_build_code.encode()) if first_build_code else 0
        static_ok = None
        compiled = None
        compile_s = None
        err = ""

        if first_build_code:
            os.makedirs(CODE_DIR, exist_ok=True)
            safe_model = prov["model"].replace("/", "_")
            code_path = os.path.join(CODE_DIR, f"{name}_{safe_model}_rep{rep}.ino")
            with open(code_path, "w", encoding="utf-8") as f:
                f.write(first_build_code)
            static_err = builder.static_check(first_build_code)
            static_ok = static_err is None
            if static_ok:
                build_dir = tempfile.mkdtemp(prefix="benchbuild_")
                try:
                    builder._write_sketch(build_dir, first_build_code)
                    t0 = time.perf_counter()
                    proc = builder._compile(build_dir, FQBN)
                    compile_s = time.perf_counter() - t0
                    compiled = proc.returncode == 0
                    if not compiled:
                        err = builder.summarize_errors(proc.stderr, proc.stdout)
                except Exception as e:
                    compiled = False
                    err = str(e)
                finally:
                    shutil.rmtree(build_dir, ignore_errors=True)
            else:
                compiled = False
                err = static_err

        row = [
            name, prov["model"], str(rep),
            str(first_build_turn or ""),
            str(envelope_valid_last), str(has_build_last),
            str(code_bytes), str(static_ok), str(compiled),
            f"{compile_s:.2f}" if compile_s is not None else "",
            csv_escape(err),
        ]
        print(",".join(row))
        sys.stderr.write(
            f"rep {rep}: first_build_turn={first_build_turn} compiled={compiled} "
            f"compile_s={compile_s}\n"
        )


if __name__ == "__main__":
    main()
