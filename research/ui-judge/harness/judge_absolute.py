"""X.4 Task 1: absolute Likert scoring of every captured frame.

The instrument is Luera et al., *MLLM as a UI Judge* (arXiv:2510.08783): the
nine factors of their Table 1, their 7-point scale, and their Figure 7 prompt.
One thing is changed, as the microstudy protocol specifies: the framing moves
from "an average user brought in to do human testing" to a user encountering a
small embedded device display, because that is the artefact being scored.

Three judges, one per model family, so no judge scores output from its own
family without a cross-check. Per-judge results are kept separate all the way
through, because a tier effect only one judge sees is not a tier effect.

  python3 judge_absolute.py <frames_dir> <out_csv> [reps]
"""

import base64
import csv
import json
import os
import re
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

import requests

sys.path.insert(0, "/home/aaron/lith-backend")
os.chdir("/home/aaron/lith-backend")
import oldowan  # noqa: E402

# Table 1, verbatim, in the paper's own order and grouping.
FACTORS = [
    ("ease_of_use",       "Ease of Use",       "The UI looks easy to use."),
    ("clarity",           "Clarity",           "The layout is uncluttered."),
    ("visual_hierarchy",  "Visual Hierarchy",  "The UI has a clear visual hierarchy."),
    ("memorable",         "Memorable",         "The UI is easily remembered."),
    ("trust",             "Trust",             "The UI appears trustworthy."),
    ("intuitive",         "Intuitive",         "The UI is intuitive."),
    ("aesthetic_pleasure", "Aesthetic Pleasure", "The UI is aesthetically pleasing."),
    ("interest",          "Interest",          "The UI is interesting."),
    ("comfort",           "Comfort",           "I feel comfortable with the UI."),
]

# Figure 7, with the single framing substitution the protocol calls for.
PROMPT = (
    "You are a user encountering a small embedded device display. For the given "
    "UI image, evaluate the following nine qualities. For each, give one of the "
    "following ratings: 1 (strongly disagree), 2 (disagree), 3 (slightly "
    "disagree), 4 (neutral), 5 (slightly agree), 6 (agree), or 7 (strongly "
    "agree), followed by a short rationale.\n\n"
    + "\n".join(f"{i}. {name}: \"{stmt}\"" for i, (_, name, stmt) in enumerate(FACTORS, 1))
    + "\n\nAnswer with one line per quality, in the order above, in exactly this "
      "form:\n**[Quality]**: **[x]/7** your rationale here."
)

# The judges: one per family, all vision-capable.
#   anthropic  claude-sonnet-5     -- the paper's Claude slot
#   openai     gpt-4.1             -- the paper's GPT-4o slot
#   groq       qwen/qwen3.6-27b    -- the paper's open-weight slot; groq no
#                                     longer hosts a Llama vision model, and an
#                                     open-weight third family matters more here
#                                     than the specific weights.
#
# max_tokens is per judge because qwen reasons in an unsuppressable <think>
# block before answering: at 1600 it spent the whole budget deliberating and
# was cut off after the first factor, which would have entered the data as
# eight missing scores rather than as the truncation it was.
JUDGES = {
    "claude-sonnet-5":  {"provider": "anthropic", "model": "claude-sonnet-5",
                         "max_tokens": 1600},
    "gpt-4.1":          {"provider": "openai",    "model": "gpt-4.1",
                         "max_tokens": 1600},
    "qwen3.6-27b":      {"provider": "groq",      "model": "qwen/qwen3.6-27b",
                         "max_tokens": 6000},
}

# Lets one judge be re-run on its own and merged back. Needed because groq's
# rate ceiling is far tighter than the other two providers', so the open-weight
# judge wants fewer workers than a run sized for anthropic and openai.
_only = os.environ.get("JUDGE_ONLY")
if _only:
    JUDGES = {k: v for k, v in JUDGES.items() if k in _only.split(",")}
WORKERS = int(os.environ.get("JUDGE_WORKERS", "8"))

_secrets = oldowan.load_secrets()
_cfg = oldowan._load_json("providers.json")
_print_lock = threading.Lock()


def post_with_retry(url, headers, body, timeout=180, tries=6):
    """POST, backing off on 429 and 5xx.

    Groq's per-minute ceiling is low enough that eight workers hit it, and a
    dropped call would enter the results as a judge that declined to score --
    indistinguishable from a judge that could not read the screen. Retrying
    keeps missing data meaning what it says.
    """
    delay = 4.0
    for attempt in range(tries):
        r = requests.post(url, headers=headers, json=body, timeout=timeout)
        if r.status_code == 429 or 500 <= r.status_code < 600:
            if attempt == tries - 1:
                r.raise_for_status()
            wait = float(r.headers.get("retry-after") or delay)
            time.sleep(min(wait, 60.0))
            delay = min(delay * 2, 60.0)
            continue
        r.raise_for_status()
        return r
    raise RuntimeError("unreachable")


def headers_for(provider):
    hs = _cfg["providers"][provider]["headers"]
    return {k: re.sub(r"\$\{([A-Za-z0-9_]+)\}",
                      lambda m: str(_secrets.get(m.group(1))
                                    or os.environ.get(m.group(1), "")), v)
            for k, v in hs.items()}


def ask(judge, img_b64):
    j = JUDGES[judge]
    prov, model = j["provider"], j["model"]
    h = headers_for(prov)
    if prov == "anthropic":
        body = {
            "model": model, "max_tokens": j["max_tokens"],
            # Thinking is on by default on sonnet-5 and is not part of the
            # instrument; the paper's judges answer directly. Low effort keeps
            # the reply a reply rather than a reasoning trace.
            "output_config": {"effort": "low"},
            "messages": [{"role": "user", "content": [
                {"type": "image", "source": {"type": "base64",
                                             "media_type": "image/png",
                                             "data": img_b64}},
                {"type": "text", "text": PROMPT}]}],
        }
        r = post_with_retry("https://api.anthropic.com/v1/messages", h, body)
        return "".join(b.get("text", "") for b in r.json()["content"]
                       if b.get("type") == "text")

    endpoint = _cfg["providers"][prov]["endpoint"]
    body = {"model": model, "max_tokens": j["max_tokens"],
            "messages": [{"role": "user", "content": [
                {"type": "text", "text": PROMPT},
                {"type": "image_url",
                 "image_url": {"url": "data:image/png;base64," + img_b64}}]}]}
    r = post_with_retry(endpoint, h, body)
    return r.json()["choices"][0]["message"]["content"]


def parse_scores(text):
    """Pull the nine ratings out of a reply.

    Tolerant on purpose: three families format the same instruction three ways,
    and qwen wraps its answer in a <think> block. A factor that cannot be found
    is left missing rather than guessed, and missing rates are reported.
    """
    text = re.sub(r"<think>.*?</think>", " ", text, flags=re.S | re.I)
    out = {}
    for key, name, _ in FACTORS:
        # the factor name, then the first number that looks like a rating
        pat = re.escape(name).replace(r"\ ", r"[\s_-]+")
        m = re.search(pat + r"\**\s*[:\-]?\s*\**\s*\[?(\d)\]?\s*(?:/\s*7)?",
                      text, re.I)
        if m and 1 <= int(m.group(1)) <= 7:
            out[key] = int(m.group(1))
    return out


def median(xs):
    xs = sorted(xs)
    n = len(xs)
    if not n:
        return None
    return xs[n // 2] if n % 2 else (xs[n // 2 - 1] + xs[n // 2]) / 2


def main():
    frames_dir, out_csv = sys.argv[1], sys.argv[2]
    reps = int(sys.argv[3]) if len(sys.argv) > 3 else 3

    frames = sorted(f for f in os.listdir(frames_dir) if f.endswith(".png"))
    imgs = {}
    for f in frames:
        with open(os.path.join(frames_dir, f), "rb") as fh:
            imgs[f] = base64.b64encode(fh.read()).decode()

    jobs = [(f, j, r) for f in frames for j in JUDGES for r in range(1, reps + 1)]
    results = {}
    done = [0]

    def work(job):
        f, j, r = job
        try:
            txt = ask(j, imgs[f])
            sc = parse_scores(txt)
        except Exception as e:  # noqa: BLE001
            sc, txt = {}, f"[error] {e}"
        with _print_lock:
            results[(f, j, r)] = (sc, txt)
            done[0] += 1
            if done[0] % 25 == 0:
                print(f"{done[0]}/{len(jobs)}", flush=True)

    with ThreadPoolExecutor(max_workers=WORKERS) as ex:
        list(ex.map(work, jobs))

    cols = ["frame", "build_id", "state", "judge", "n_reps_parsed"] \
        + [k for k, _, _ in FACTORS]
    with open(out_csv, "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(cols)
        for f in frames:
            build_id, _, state = f[:-4].partition("__")
            for j in JUDGES:
                per = [results[(f, j, r)][0] for r in range(1, reps + 1)]
                row = [f, build_id, state, j,
                       sum(1 for p in per if len(p) == len(FACTORS))]
                for k, _, _ in FACTORS:
                    vals = [p[k] for p in per if k in p]
                    row.append(median(vals) if vals else "")
                w.writerow(row)

    # Raw replies kept beside the scores: the rationales are the only place the
    # judges say *why*, and a score with no trace back to a reply cannot be
    # checked afterwards.
    with open(out_csv.replace(".csv", "_raw.jsonl"), "w", encoding="utf-8") as fh:
        for (f, j, r), (sc, txt) in sorted(results.items()):
            fh.write(json.dumps({"frame": f, "judge": j, "rep": r,
                                 "scores": sc, "reply": txt}) + "\n")
    print("wrote", out_csv)


if __name__ == "__main__":
    main()
