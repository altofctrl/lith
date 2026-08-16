"""Gate 2 + Gate 3 over a whole corpus: build each sketch into the journey
harness, run it, and collect the six canonical frames.

  python batch_render.py <code_dir> <frames_dir> <out_csv>

Two failure modes are recorded rather than hidden, because they mean different
things. `sim_build_ok` false means the sketch would not compile against the host
shims at all -- a build that arduino-cli accepted can still land here if it
leans on something the shim does not carry, and that is the harness's fault, not
the model's, so those rows are excluded from judging and reported as harness
coverage. `ran_ok` false means it built and then hung or crashed while being
driven, which is the model's.
"""

import csv
import os
import re
import shutil
import subprocess
import sys
import time

# Keywords that begin a statement or a declaration, never a function definition.
_NOT_FUNC = {
    "if", "for", "while", "switch", "else", "do", "return", "struct", "class",
    "enum", "namespace", "union", "typedef", "template", "operator", "extern",
    "case", "catch", "try", "sizeof", "new", "delete", "using", "public",
    "private", "protected", "#define", "#if", "#endif",
}

# A top-level function definition: return type, name, parenthesised args, then
# an opening brace, all starting at column 0.
_FUNC_DEF = re.compile(
    r"^((?:[A-Za-z_][\w:]*(?:\s*<[^;{]*?>)?[\s*&]+)+)([A-Za-z_]\w*)\s*"
    r"\(([^;{)]*)\)\s*(?:const\s*)?\{",
    re.M)

# Anything that introduces a type a later signature might mention.
_TYPE_DECL = re.compile(r"\b(?:struct|class|enum|union|typedef|using)\b")


def _strip_and_depth(code):
    """Blank out comments, literals and preprocessor lines; return brace depth.

    Same length as the input, so offsets into the returned text are offsets into
    the original. Depth is the brace nesting *before* the character at each
    index, which is what tells a top-level function from a class member.
    """
    out = list(code)
    depth = [0] * (len(code) + 1)
    i, n, d = 0, len(code), 0
    while i < n:
        c = code[i]
        two = code[i:i + 2]
        if two == "//":
            while i < n and code[i] != "\n":
                out[i] = " "
                depth[i] = d
                i += 1
            continue
        if two == "/*":
            while i < n and code[i:i + 2] != "*/":
                if code[i] != "\n":
                    out[i] = " "
                depth[i] = d
                i += 1
            for _ in range(min(2, n - i)):
                out[i] = " "
                depth[i] = d
                i += 1
            continue
        if c == "#" and (i == 0 or code[i - 1] == "\n"):
            # a preprocessor line, continuations included
            while i < n:
                out[i] = " " if code[i] != "\n" else "\n"
                depth[i] = d
                if code[i] == "\n" and not code[max(0, i - 1)] == "\\":
                    i += 1
                    break
                i += 1
            continue
        if c in "\"'":
            q = c
            depth[i] = d
            out[i] = " "
            i += 1
            while i < n:
                depth[i] = d
                if code[i] == "\\":
                    out[i] = " "
                    i += 1
                    if i < n:
                        out[i] = " "
                        depth[i] = d
                        i += 1
                    continue
                done = code[i] == q
                out[i] = " " if code[i] != "\n" else "\n"
                i += 1
                if done:
                    break
            continue
        depth[i] = d
        if c == "{":
            d += 1
        elif c == "}":
            d = max(0, d - 1)
        i += 1
    depth[n] = d
    return "".join(out), depth


def add_prototypes(code):
    """Insert forward declarations, as the Arduino builder does.

    A .ino is not a C++ translation unit. Before compiling, arduino-cli
    generates a prototype for every function in the sketch, which is why a
    sketch may call a function defined further down the file and still build.
    Including the .ino raw does not do that, so a sketch that would build for
    the device fails here for a reason that has nothing to do with the model.

    The prototypes go immediately *before the first function definition* rather
    than at the top of the file, which is where arduino-cli puts them. That
    placement is deliberate and is the one place this harness knowingly differs
    from the real toolchain: arduino-cli's top-of-file placement is what
    produces the "'Button' was not declared in this scope" failures seen in
    Gate 1, because the prototypes land above the struct definitions they
    mention. Gate 1 has already measured that; Gate 2 asks a different
    question -- what does this UI look like -- and reproducing the prototype
    bug here would only stop screens from being seen.
    """
    clean, depth = _strip_and_depth(code)
    defs = []
    for m in _FUNC_DEF.finditer(clean):
        # Only true top-level definitions. A member function inside the sketch's
        # own `class LGFX : public lgfx::LGFX_Device` matches the same shape,
        # and hoisting a prototype for it -- or worse, inserting the whole block
        # inside the class body -- redeclares the constructor and breaks a
        # sketch that was fine.
        if depth[m.start()] != 0:
            continue
        ret, name, args = m.group(1).strip(), m.group(2), m.group(3)
        if ret.split()[0] in _NOT_FUNC or name in _NOT_FUNC:
            continue
        if name in ("setup", "loop"):
            continue
        defs.append((m.start(), f"{ret} {name}({args});"))
    if not defs:
        return code

    # Where the block goes. Not simply "before the first function": a sketch
    # often declares an enum or struct part-way down and uses it in a later
    # signature, and hoisting that signature above its type reproduces exactly
    # the arduino-cli failure this is meant to avoid ("'BtnEvent' does not name
    # a type"). So the block goes after the last top-level type declaration, at
    # the first function definition from there on. Functions defined earlier
    # need no prototype: their own definitions already precede every call.
    last_type = 0
    for m in _TYPE_DECL.finditer(clean):
        if depth[m.start()] == 0:
            last_type = max(last_type, m.end())
    at = next((p for p, _ in defs if p >= last_type), defs[0][0])
    defs = [(p, d) for p, d in defs if p >= at]
    if not defs:
        return code
    block = ("\n// --- prototypes generated by the harness, as arduino-cli does\n"
             + "\n".join(d for _, d in defs) + "\n\n")
    return code[:at] + block + code[at:]

HERE = os.path.dirname(os.path.abspath(__file__))
SIM = os.path.join(HERE, "sim")
BUILD = os.path.join(SIM, "build_one.sh")

# A sketch gets four minutes of wall clock to walk the journey. The harness has
# its own 240 s budget and stops itself first for anything it can interrupt;
# this outer limit is for a build that blocks inside its own loop(), which
# nothing inside the process can break out of.
RUN_TIMEOUT_S = 300
BUILD_TIMEOUT_S = 300

STATES = ["1_boot_idle", "2_started", "3_mid_meeting",
          "4_threshold", "5_extended", "6_stopped"]


def sh(cmd, timeout):
    """Run under Git Bash, which is what build_one.sh expects."""
    return subprocess.run(["C:/Program Files/Git/bin/bash.exe", "-lc", cmd],
                          capture_output=True, text=True, timeout=timeout)


def parse_report(stdout):
    rep = {"frames": {}}
    for line in stdout.splitlines():
        parts = line.rstrip("\n").split("\t")
        if not parts:
            continue
        if parts[0] == "frame" and len(parts) >= 5:
            rep["frames"][parts[1]] = {
                "present": parts[2] == "1", "hash": parts[3], "ink": float(parts[4])}
        elif len(parts) >= 2:
            rep[parts[0]] = parts[1]
    return rep


def main():
    # Absolute throughout: build_one.sh cds to its own directory, so a relative
    # sketch path resolves against the harness rather than the caller.
    code_dir = os.path.abspath(sys.argv[1])
    frames_dir = os.path.abspath(sys.argv[2])
    out_csv = os.path.abspath(sys.argv[3])
    os.makedirs(frames_dir, exist_ok=True)
    exe_dir = os.path.join(os.environ.get("TEMP", "/tmp"), "knapp_sim_exe")
    os.makedirs(exe_dir, exist_ok=True)
    prep_dir = os.path.join(os.path.dirname(out_csv), "prepared_" +
                            os.path.basename(frames_dir))
    os.makedirs(prep_dir, exist_ok=True)

    rows = []
    for fn in sorted(os.listdir(code_dir)):
        if not fn.endswith(".ino"):
            continue
        build_id = fn[:-4]
        src = os.path.join(code_dir, fn)
        # The prepared copy, with prototypes, is what gets compiled -- and is
        # kept, so any frame can be traced back to the exact text that drew it.
        prepared = os.path.join(prep_dir, build_id + ".cpp")
        with open(src, encoding="utf-8") as fh:
            code = fh.read()
        with open(prepared, "w", encoding="utf-8") as fh:
            fh.write(add_prototypes(code))
        ino = prepared.replace("\\", "/")
        exe = os.path.join(exe_dir, build_id + ".exe").replace("\\", "/")

        row = {"build_id": build_id, "sim_build_ok": False, "ran_ok": False,
               "timed_out": "", "init_called": "", "rotation_set": "",
               "rotation": "", "panel_w": "", "panel_h": "", "loops": "",
               "motor_writes": "", "frames_present": 0, "distinct_frames": 0,
               "blank_frames": 0, "note": ""}

        t0 = time.time()
        try:
            b = sh(f'"{BUILD}" "{ino}" "{exe}"', BUILD_TIMEOUT_S)
        except subprocess.TimeoutExpired:
            row["note"] = "sim build timed out"
            rows.append(row)
            print(build_id, "BUILD TIMEOUT", flush=True)
            continue
        if b.returncode != 0:
            # First compiler diagnostic only: enough to tell a shim gap from a
            # broken sketch without carrying a page of template noise.
            first = next((l for l in b.stderr.splitlines() if "error:" in l), "")
            row["note"] = " ".join(first.split())[:250]
            rows.append(row)
            print(build_id, "sim build failed:", row["note"][:90], flush=True)
            continue
        row["sim_build_ok"] = True

        try:
            r = sh(f'"{exe}" "{frames_dir.replace(chr(92), "/")}" "{build_id}"',
                   RUN_TIMEOUT_S)
            out = r.stdout
            row["ran_ok"] = r.returncode == 0
            if r.returncode != 0:
                row["note"] = f"exit {r.returncode}"
        except subprocess.TimeoutExpired as e:
            out = (e.stdout or "")
            if isinstance(out, bytes):
                out = out.decode("utf-8", "replace")
            row["note"] = "run timed out (blocked inside loop)"
            print(build_id, "RUN TIMEOUT", flush=True)

        rep = parse_report(out)
        for k in ("timed_out", "init_called", "rotation_set", "rotation",
                  "panel_w", "panel_h", "loops", "motor_writes",
                  "distinct_frames"):
            if k in rep:
                row[k] = rep[k]
        present = [s for s in STATES if rep["frames"].get(s, {}).get("present")]
        row["frames_present"] = len(present)
        # A frame of one flat colour is a screen the build never drew on. It is
        # not the same as an absent frame and is counted separately, since a
        # judge shown a blank panel would be scoring nothing at all.
        row["blank_frames"] = sum(
            1 for s in present if rep["frames"][s]["ink"] < 0.001)
        for s in STATES:
            row["ink_" + s] = round(rep["frames"].get(s, {}).get("ink", 0.0), 4)

        rows.append(row)
        print(f'{build_id}: sim_build=1 ran={int(row["ran_ok"])} '
              f'frames={row["frames_present"]} distinct={row["distinct_frames"]} '
              f'blank={row["blank_frames"]} {time.time()-t0:.0f}s', flush=True)

    cols = ["build_id", "sim_build_ok", "ran_ok", "timed_out", "init_called",
            "rotation_set", "rotation", "panel_w", "panel_h", "loops",
            "motor_writes", "frames_present", "distinct_frames", "blank_frames"] \
        + ["ink_" + s for s in STATES] + ["note"]
    with open(out_csv, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=cols, extrasaction="ignore")
        w.writeheader()
        for r in rows:
            w.writerow(r)
    print("wrote", out_csv)


if __name__ == "__main__":
    main()
