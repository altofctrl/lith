"""X.4 Task 2: within-provider pairwise preference.

This is the primary evidence, not the Likert scores. Luera et al. find absolute
score prediction weak (exact accuracy 35-38%) while pairwise agreement is much
stronger once two options genuinely differ, so the microstudy reads the pairwise
results as the finding and the absolute scores as description.

Their Task 2 prompt is used with the same single framing substitution as Task 1,
and their ten criteria: the nine Table 1 factors plus an overall preference.

Every pair is run twice with the two builds swapped between the UI-A and UI-B
slots, and only agreement across both orders counts as a preference. An MLLM
shown two images has a position bias; without the swap, a bias toward the first
slot is indistinguishable from a real preference, which would show up here as a
tier effect that is really an artefact of which build got listed first.

  python3 judge_pairwise.py <frames_dir> <render_csv> <out_csv>
"""

import base64
import csv
import itertools
import json
import os
import re
import sys
import threading
from concurrent.futures import ThreadPoolExecutor

import requests

sys.path.insert(0, "/home/aaron/lith-backend")
os.chdir("/home/aaron/lith-backend")
import oldowan  # noqa: E402

from judge_absolute import (FACTORS, JUDGES, headers_for, post_with_retry,
                            _cfg)  # noqa: E402

CRITERIA = [(k, n) for k, n, _ in FACTORS] + [("overall", "Overall Preference")]

PROMPT = (
    "You are a user evaluating the display of a small embedded device. You will "
    "be shown two UI screenshots, UI-A and UI-B, of the same moment in the same "
    "task. Your task is to determine which UI an ordinary person would prefer "
    "for the following evaluation criteria.\n\n"
    + "\n".join(f"{i}. {n}" for i, (_, n) in enumerate(CRITERIA, 1))
    + "\n\nAnswer with one block per criterion, in the order above, in exactly "
      "this form:\n<criterion>[text]</criterion><result>[UI-A or UI-B]</result>"
      "<reason>[reasoning in less than 50 words]</reason>"
)

_lock = threading.Lock()

# Ten criteria with a rationale each, and for qwen a <think> block before any of
# it. The absolute task showed what happens when the budget runs out mid-answer:
# the missing criteria enter the data as absent preferences rather than as the
# truncation they are.
PAIR_TOKENS = {"claude-sonnet-5": 2400, "gpt-4.1": 2400, "qwen3.6-27b": 9000}


def ask_pair(judge, a_b64, b_b64):
    j = JUDGES[judge]
    prov, model = j["provider"], j["model"]
    h = headers_for(prov)
    if prov == "anthropic":
        body = {"model": model, "max_tokens": PAIR_TOKENS[judge],
                "output_config": {"effort": "low"},
                "messages": [{"role": "user", "content": [
                    {"type": "text", "text": "UI-A:"},
                    {"type": "image", "source": {"type": "base64",
                                                 "media_type": "image/png",
                                                 "data": a_b64}},
                    {"type": "text", "text": "UI-B:"},
                    {"type": "image", "source": {"type": "base64",
                                                 "media_type": "image/png",
                                                 "data": b_b64}},
                    {"type": "text", "text": PROMPT}]}]}
        r = post_with_retry("https://api.anthropic.com/v1/messages", h, body,
                            timeout=240)
        return "".join(x.get("text", "") for x in r.json()["content"]
                       if x.get("type") == "text")

    body = {"model": model, "max_tokens": PAIR_TOKENS[judge],
            "messages": [{"role": "user", "content": [
                {"type": "text", "text": "UI-A:"},
                {"type": "image_url",
                 "image_url": {"url": "data:image/png;base64," + a_b64}},
                {"type": "text", "text": "UI-B:"},
                {"type": "image_url",
                 "image_url": {"url": "data:image/png;base64," + b_b64}},
                {"type": "text", "text": PROMPT}]}]}
    r = post_with_retry(_cfg["providers"][prov]["endpoint"], h, body,
                        timeout=240)
    return r.json()["choices"][0]["message"]["content"]


def parse_pair(text):
    """Pull the ten preferences out of a reply.

    Anchored on <result> rather than on <criterion>, because gpt-4.1 routinely
    names the tag after the criterion instead -- `<ease of use>[UI-A]</ease of
    use><result>UI-A</result>` -- which a <criterion>-only pattern misses
    entirely. That cost 18% of one judge's comparisons on the first run, and
    they would have entered the analysis as an absent preference rather than as
    a formatting difference. Each <result> is attributed to the criterion named
    last in the text before it, which is the tag immediately preceding it.
    """
    text = re.sub(r"<think>.*?</think>", " ", text, flags=re.S | re.I)
    parts = re.split(r"<result>(.*?)</result>", text, flags=re.S | re.I)
    out = {}
    for i in range(1, len(parts), 2):
        pre, res = parts[i - 1], parts[i]
        pick = "A" if re.search(r"ui[\s\-_]*a", res, re.I) else (
            "B" if re.search(r"ui[\s\-_]*b", res, re.I) else None)
        if pick is None:
            continue
        best, best_pos = None, -1
        for key, name in CRITERIA:
            pat = re.escape(name).replace(r"\ ", r"[\s_-]+")
            for m in re.finditer(pat, pre, re.I):
                if m.start() > best_pos:
                    best_pos, best = m.start(), key
        if best is not None and best not in out:
            out[best] = pick
    return out


def main():
    frames_dir, render_csv, out_csv = sys.argv[1], sys.argv[2], sys.argv[3]

    # Only builds that actually produced frames take part.
    with open(render_csv, encoding="utf-8") as fh:
        renders = {r["build_id"]: r for r in csv.DictReader(fh)
                   if r["ran_ok"] in ("True", "1") and int(r["frames_present"] or 0) > 0}

    by_provider = {}
    for b in renders:
        by_provider.setdefault(b.split("_", 1)[0], []).append(b)

    frames = {f[:-4]: f for f in os.listdir(frames_dir) if f.endswith(".png")}
    imgs = {}

    def img(build, state):
        key = f"{build}__{state}"
        if key not in frames:
            return None
        if key not in imgs:
            with open(os.path.join(frames_dir, frames[key]), "rb") as fh:
                imgs[key] = base64.b64encode(fh.read()).decode()
        return imgs[key]

    states = sorted({f.split("__")[1] for f in frames})

    jobs = []
    for prov, builds in by_provider.items():
        for a, b in itertools.combinations(sorted(builds), 2):
            for st in states:
                if img(a, st) and img(b, st):
                    for j in JUDGES:
                        jobs.append((prov, a, b, st, j))

    results = {}
    raws = {}
    done = [0]

    def work(job):
        prov, a, b, st, j = job
        raw_f = raw_r = ""
        try:
            raw_f = ask_pair(j, img(a, st), img(b, st))
            fwd = parse_pair(raw_f)
            # swapped: a is now in the UI-B slot
            raw_r = ask_pair(j, img(b, st), img(a, st))
            rev = parse_pair(raw_r)
        except Exception as e:  # noqa: BLE001
            fwd, rev = {}, {"__error__": str(e)}
        raws[job] = (raw_f, raw_r)
        with _lock:
            results[job] = (fwd, rev)
            done[0] += 1
            if done[0] % 20 == 0:
                print(f"{done[0]}/{len(jobs)}", flush=True)

    with ThreadPoolExecutor(max_workers=6) as ex:
        list(ex.map(work, jobs))

    with open(out_csv, "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["provider", "build_a", "build_b", "state", "judge",
                    "criterion", "winner", "consistent"])
        for (prov, a, b, st, j), (fwd, rev) in sorted(results.items()):
            for key, _ in CRITERIA:
                f_pick, r_pick = fwd.get(key), rev.get(key)
                if not f_pick or not r_pick:
                    continue
                # forward: A slot = build a. reverse: A slot = build b.
                f_win = a if f_pick == "A" else b
                r_win = b if r_pick == "A" else a
                consistent = f_win == r_win
                # An inconsistent pair is a position flip, not a preference, and
                # is recorded as a tie rather than resolved by coin toss.
                w.writerow([prov, a, b, st, j, key,
                            f_win if consistent else "tie", consistent])

    with open(out_csv.replace(".csv", "_raw.jsonl"), "w", encoding="utf-8") as fh:
        for k, v in sorted(results.items()):
            fh.write(json.dumps({"job": list(k), "forward": v[0],
                                 "reverse": v[1]}) + "\n")
    print("wrote", out_csv, len(jobs), "pair-jobs")


if __name__ == "__main__":
    main()
