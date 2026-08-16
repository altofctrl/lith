"""Gate 1b: the as-deployed compile gate, with Oldowan's hidden repair loop.

bench_build.py measures whether a model's *first* sketch compiles. That is not
what a workshop participant experiences. In production, builder.py never shows a
compile error to the user: it hands the errors back to Oldowan as a hidden
`[internal]` message and reflashes the corrected sketch, up to MAX_ATTEMPTS = 3
passes, behind craft-flavoured status lines. Since the microstudy's stated
object is "`oldowan` as deployed", the corpus that gets rendered and judged has
to be the one a user would actually end up holding.

So Gate 1 is reported twice: first-pass compile (gate1_compile.py) and
as-deployed compile (here). The repair prompt, the attempt ceiling and the
compile call are taken from builder.py rather than reimplemented, so the only
difference from production is the reconstructed history noted below.

Usage: python3 gate1_repair.py <code_dir> <out_dir> > gate1_repair.csv
"""

import csv
import json
import os
import re
import shutil
import sys
import tempfile
import time

sys.path.insert(0, "/home/aaron/lith-backend")
os.chdir("/home/aaron/lith-backend")

import builder
import oldowan

from gate1_compile import MATRIX, split_name

# Verbatim from bench_latency.py / bench_build.py, so the reconstructed history
# is the same conversation every model in the matrix actually saw.
REAL_USER_TURNS = [
    "a meeting cost meter i can start when a meeting starts",
    "silent, just the light",
    "what next?",
]

MAX_ATTEMPTS = builder.MAX_ATTEMPTS


def load_named_provider(name, model):
    cfg = oldowan._load_json("providers.json")
    prov = dict(cfg["providers"][name])
    prov["_name"] = name
    prov["_timeout"] = cfg.get("request_timeout_s", 300)
    prov["model"] = model
    secrets = oldowan.load_secrets()

    def expand(v):
        return re.sub(r"\$\{([A-Za-z0-9_]+)\}",
                      lambda m: str(secrets.get(m.group(1))
                                    or os.environ.get(m.group(1), "")), v)

    prov["headers"] = {k: expand(v) for k, v in prov.get("headers", {}).items()}

    # providers.json carries one block per provider, tuned for the model that
    # block normally runs. The anthropic block sets effort:medium for
    # claude-sonnet-5; claude-haiku-4-5 rejects the effort parameter outright
    # with a 400, so a tier sweep has to drop it rather than inherit it. Left
    # in place for the tiers that do support it, since removing it everywhere
    # would change how the mid and high tiers think and stop being production
    # defaults.
    if prov.get("format") == "anthropic" and "haiku" in model:
        prov.pop("effort", None)
    return prov


def _call_openai_compat(prov, messages):
    import requests
    reasoning = prov["model"].startswith(("gpt-5", "o1", "o3", "o4"))
    body = {
        "model": prov["model"],
        ("max_completion_tokens" if reasoning else "max_tokens"):
            prov.get("max_tokens", 8000),
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
    return r.json()["choices"][0]["message"]["content"]


def call(prov, messages):
    """Always returns reply text.

    oldowan._call_anthropic returns {text, stop_reason, usage} because the
    production path needs the stop reason to tell a truncated sketch from a bad
    one; the openai-compatible path returns a bare string. Normalising here
    rather than at the call sites is what bench_build.py omits, which is why its
    anthropic rows report an invalid envelope for replies that were fine.
    """
    if prov["format"] == "anthropic":
        res = oldowan._call_anthropic(prov, messages)
        if res.get("stop_reason") == "max_tokens":
            raise RuntimeError("hit max_tokens; sketch cut off mid-write")
        return res["text"]
    return _call_openai_compat(prov, messages)


def try_compile(code, fqbn):
    """Returns (ok, error_summary). Same static check + compile builder runs."""
    err = builder.static_check(code)
    if err is not None:
        return False, err
    bd = tempfile.mkdtemp(prefix="gate1r_")
    try:
        builder._write_sketch(bd, code)
        proc = builder._compile(bd, fqbn)
        if proc.returncode == 0:
            return True, ""
        return False, builder.summarize_errors(proc.stderr, proc.stdout)
    except Exception as e:  # noqa: BLE001
        return False, str(e)
    finally:
        shutil.rmtree(bd, ignore_errors=True)


def main():
    code_dir, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    fqbn = oldowan.device_profile()["fqbn"]

    w = csv.writer(sys.stdout)
    w.writerow(["build_id", "provider", "tier", "model", "rep",
                "passes_used", "deployed_compiled", "final_code_bytes",
                "repair_error"])

    for fn in sorted(os.listdir(code_dir)):
        if not fn.endswith(".ino"):
            continue
        stem = fn[:-4]
        key, rep = split_name(stem)
        provider, tier = MATRIX.get(key, (key.split("_")[0], "?"))
        model = key.split("_", 1)[1].replace("_", "/")   # groq_openai_gpt-oss-120b

        code = open(os.path.join(code_dir, fn), encoding="utf-8").read()
        ok, err = try_compile(code, fqbn)
        passes = 1
        note = ""

        if not ok:
            prov = load_named_provider(provider, model)
            # Reconstructed history. Production carries the model's own
            # intermediate asks as well; here the record is the three real user
            # turns plus the model's own final build envelope, which is the
            # part the repair actually reasons over. Noted as a deviation.
            history = []
            for t in REAL_USER_TURNS:
                history.append({"role": "user", "content": t})
            history.append({"role": "assistant", "content": json.dumps(
                {"say": "here's the knapp.",
                 "build": {"name": "meeting cost meter", "code": code}})})

            while passes < MAX_ATTEMPTS and not ok:
                history.append({"role": "user", "content":
                                "[internal] the sketch failed to compile. Errors:\n"
                                + err + "\nReply with the corrected complete build."})
                try:
                    reply = call(prov, history)
                except Exception as e:  # noqa: BLE001
                    note = f"agent error during repair: {e}"
                    break
                history.append({"role": "assistant", "content": reply})
                try:
                    env = oldowan.parse_envelope(reply)
                except Exception as e:  # noqa: BLE001
                    note = f"unparseable repair envelope: {e}"
                    break
                if not env or not env.get("build"):
                    note = "repair turn returned no build"
                    break
                code = env["build"]["code"]
                passes += 1
                ok, err = try_compile(code, fqbn)

        if ok:
            with open(os.path.join(out_dir, fn), "w", encoding="utf-8") as f:
                f.write(code)

        w.writerow([stem, provider, tier, model, rep, passes, ok,
                    len(code.encode()),
                    " ".join((note or err or "").split())[:300]])
        sys.stdout.flush()
        sys.stderr.write(f"{stem}: passes={passes} deployed_compiled={ok}\n")


if __name__ == "__main__":
    main()
