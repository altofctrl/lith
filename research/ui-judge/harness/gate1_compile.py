"""Gate 1: compile every sketch in the corpus with arduino-cli, uniformly.

The earlier bench_build.py CSVs are inconsistent with the .ino corpus that sits
beside them (rows reporting no build for models that nonetheless have sketch
files on disk), because they were written across several partial runs with
different nudge settings. This re-runs the compile gate alone, over whatever
.ino files are actually present, so Gate 1 is one internally consistent table.

Usage: python3 gate1_compile.py <code_dir> > gate1.csv
"""

import csv
import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, "/home/aaron/lith-backend")
os.chdir("/home/aaron/lith-backend")

import builder
import oldowan

# provider prefix -> (provider, tier) for the resolved matrix actually run
MATRIX = {
    "anthropic_claude-haiku-4-5-20251001": ("anthropic", "low"),
    "anthropic_claude-sonnet-5": ("anthropic", "mid"),
    "anthropic_claude-opus-5": ("anthropic", "high"),
    "openai_gpt-4o-mini": ("openai", "low"),
    "openai_gpt-4.1": ("openai", "mid"),
    "openai_gpt-5.5": ("openai", "high"),
    "groq_llama-3.1-8b-instant": ("groq", "low"),
    "groq_llama-3.3-70b-versatile": ("groq", "mid"),
    "groq_openai_gpt-oss-120b": ("groq", "high"),
    "deepseek_deepseek-v4-flash": ("deepseek", "low"),
    "deepseek_deepseek-v4-pro": ("deepseek", "high"),
}


def split_name(stem):
    """`anthropic_claude-opus-5_rep1` -> (key, rep)."""
    key, _, rep = stem.rpartition("_rep")
    return key, int(rep)


def main():
    code_dir = sys.argv[1]
    fqbn = oldowan.device_profile()["fqbn"]

    w = csv.writer(sys.stdout)
    w.writerow(["build_id", "provider", "tier", "model", "rep", "code_bytes",
                "static_check_ok", "compiled", "compile_s", "error_summary"])

    for fn in sorted(os.listdir(code_dir)):
        if not fn.endswith(".ino"):
            continue
        stem = fn[:-4]
        key, rep = split_name(stem)
        provider, tier = MATRIX.get(key, (key.split("_")[0], "?"))
        model = key.split("_", 1)[1]

        code = open(os.path.join(code_dir, fn), encoding="utf-8").read()
        static_err = builder.static_check(code)
        static_ok = static_err is None
        compiled, compile_s, err = False, "", (static_err or "")

        if static_ok:
            bd = tempfile.mkdtemp(prefix="gate1_")
            try:
                builder._write_sketch(bd, code)
                t0 = time.perf_counter()
                proc = builder._compile(bd, fqbn)
                compile_s = f"{time.perf_counter() - t0:.2f}"
                compiled = proc.returncode == 0
                if not compiled:
                    err = builder.summarize_errors(proc.stderr, proc.stdout)
            except Exception as e:  # noqa: BLE001
                err = str(e)
            finally:
                shutil.rmtree(bd, ignore_errors=True)

        err = " ".join((err or "").split())[:400]
        w.writerow([stem, provider, tier, model, rep, len(code.encode()),
                    static_ok, compiled, compile_s, err])
        sys.stdout.flush()
        sys.stderr.write(f"{stem}: compiled={compiled}\n")


if __name__ == "__main__":
    main()
