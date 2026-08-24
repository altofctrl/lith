"""Compile queue for knapps.

One worker thread compiles agent-generated sketches with arduino-cli.
On compile failure it runs the internal repair loop: errors go back to
Oldowan as [internal] messages (hidden from the user), the corrected
sketch is retried, up to MAX_ATTEMPTS. The user only ever sees
queued -> shaping -> ready | failed, plus which pass we are on.

Outputs per build (builds/<id>/out/): bootloader, partitions,
boot_app0 and app binaries, plus an esp-web-tools manifest.json so
the browser can flash over Web Serial.
"""

import glob
import json
import os
import queue
import re
import shutil
import subprocess
import threading
import time

import oldowan

BASE = os.path.dirname(os.path.abspath(__file__))
BUILDS = os.path.join(BASE, "builds")
SESSIONS = os.path.join(BASE, "sessions")
ARDUINO = os.path.expanduser("~/.local/bin/arduino-cli")

MAX_ATTEMPTS = 3
COMPILE_TIMEOUT_S = 420
MAX_CODE_BYTES = 200_000

os.makedirs(BUILDS, exist_ok=True)
os.makedirs(SESSIONS, exist_ok=True)

_q = queue.Queue()
state_lock = threading.RLock()  # guards session + build meta files


# ------------------------------------------------------------- persistence

def _meta_path(build_id):
    return os.path.join(BUILDS, build_id, "meta.json")


def read_meta(build_id):
    with state_lock:
        path = _meta_path(build_id)
        if not os.path.exists(path):
            return None
        with open(path, encoding="utf-8") as f:
            return json.load(f)


def write_meta(build_id, meta):
    with state_lock:
        with open(_meta_path(build_id), "w", encoding="utf-8") as f:
            json.dump(meta, f, indent=1)


def session_path(session_id):
    return os.path.join(SESSIONS, session_id + ".json")


def read_session(session_id):
    with state_lock:
        path = session_path(session_id)
        if not os.path.exists(path):
            return None
        with open(path, encoding="utf-8") as f:
            return json.load(f)


def write_session(session_id, sess):
    with state_lock:
        with open(session_path(session_id), "w", encoding="utf-8") as f:
            json.dump(sess, f, indent=1)


# ------------------------------------------------------------------ checks

def toolchain_ready():
    try:
        out = subprocess.run([ARDUINO, "core", "list"], capture_output=True,
                             text=True, timeout=30).stdout
        return "esp32:esp32" in out
    except Exception:
        return False


def static_check(code):
    if len(code.encode()) > MAX_CODE_BYTES:
        return "sketch too large"
    if "void setup" not in code or "void loop" not in code:
        return "sketch must define setup() and loop()"
    return None


def summarize_errors(stderr, stdout):
    text = (stderr or "") + "\n" + (stdout or "")
    lines = [ln.strip() for ln in text.splitlines()
             if "error:" in ln or "undefined reference" in ln or "fatal error" in ln]
    if lines:
        return "\n".join(lines[:30])
    return text[-2000:]


# ----------------------------------------------------------------- compile

def _compile(build_dir, fqbn):
    sketch = os.path.join(build_dir, "knapp")
    out = os.path.join(build_dir, "out")
    proc = subprocess.run(
        [ARDUINO, "compile", "--fqbn", fqbn, "--output-dir", out, sketch],
        capture_output=True, text=True, timeout=COMPILE_TIMEOUT_S,
    )
    return proc


def _finalize(build_dir, name, chip_family, offsets):
    """Copy boot_app0 in and write the esp-web-tools manifest."""
    out = os.path.join(build_dir, "out")
    boot_app0 = sorted(glob.glob(os.path.expanduser(
        "~/.arduino15/packages/esp32/hardware/esp32/*/tools/partitions/boot_app0.bin")))
    if boot_app0:
        shutil.copy(boot_app0[-1], os.path.join(out, "boot_app0.bin"))
    parts = [
        {"path": "knapp.ino.bootloader.bin", "offset": int(offsets["bootloader"], 16)},
        {"path": "knapp.ino.partitions.bin", "offset": int(offsets["partitions"], 16)},
        {"path": "boot_app0.bin", "offset": int(offsets["boot_app0"], 16)},
        {"path": "knapp.ino.bin", "offset": int(offsets["app"], 16)},
    ]
    parts = [p for p in parts if os.path.exists(os.path.join(out, p["path"]))]
    manifest = {
        "name": "your knapp · " + name,
        "version": "1",
        "new_install_prompt_erase": True,
        "builds": [{"chipFamily": chip_family, "parts": parts}],
    }
    with open(os.path.join(out, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)


# ------------------------------------------------------------------ worker

def _write_sketch(build_dir, code):
    sketch_dir = os.path.join(build_dir, "knapp")
    os.makedirs(sketch_dir, exist_ok=True)
    with open(os.path.join(sketch_dir, "knapp.ino"), "w", encoding="utf-8") as f:
        f.write(code)


def _work_one(build_id):
    meta = read_meta(build_id)
    if meta is None:
        return
    profile = oldowan.device_profile()
    build_dir = os.path.join(BUILDS, build_id)
    code = meta["code"]
    session_id = meta["session_id"]

    for attempt in range(1, MAX_ATTEMPTS + 1):
        meta.update(status="compiling" if attempt == 1 else "repairing",
                    attempt=attempt)
        write_meta(build_id, meta)

        err = static_check(code)
        if err is None:
            _write_sketch(build_dir, code)
            try:
                proc = _compile(build_dir, profile["fqbn"])
            except subprocess.TimeoutExpired:
                err = "compile timed out"
            else:
                if proc.returncode == 0:
                    _finalize(build_dir, meta["name"], profile["chip_family"],
                              profile["flash_offsets"])
                    meta.update(status="ready", error=None,
                                finished=time.time())
                    write_meta(build_id, meta)
                    return
                err = summarize_errors(proc.stderr, proc.stdout)

        # hidden repair loop: hand the errors back to oldowan
        if attempt >= MAX_ATTEMPTS:
            break
        sess = read_session(session_id)
        if sess is None:
            break
        sess["messages"].append({
            "role": "user", "hidden": True,
            "content": "[internal] the sketch failed to compile. Errors:\n"
                       + err + "\nReply with the corrected complete build.",
        })
        try:
            env = oldowan.chat(sess["messages"])
        except Exception as e:
            meta.update(status="failed", error="agent error during repair: " + str(e))
            write_meta(build_id, meta)
            write_session(session_id, sess)
            return
        sess["messages"].append({"role": "assistant", "hidden": True,
                                 "content": json.dumps(env)})
        write_session(session_id, sess)
        if not env.get("build"):
            break
        code = env["build"]["code"]
        meta["code"] = code

    meta.update(status="failed",
                error=err,
                say="that one fractured along a hidden flaw. i couldn't get a "
                    "clean edge after a few tries; describe it differently, or "
                    "simplify one part, and we'll strike again.")
    write_meta(build_id, meta)


def _worker():
    while True:
        build_id = _q.get()
        try:
            _work_one(build_id)
        except Exception as e:
            meta = read_meta(build_id) or {}
            meta.update(status="failed", error="builder crashed: " + str(e))
            try:
                write_meta(build_id, meta)
            except Exception:
                pass
        finally:
            _q.task_done()


def start_worker():
    t = threading.Thread(target=_worker, daemon=True, name="knapp-builder")
    t.start()


def enqueue(session_id, name, code):
    build_id = "%d-%s" % (int(time.time() * 1000), re.sub(r"[^a-z0-9-]", "", name)[:24])
    build_dir = os.path.join(BUILDS, build_id)
    os.makedirs(os.path.join(build_dir, "out"), exist_ok=True)
    write_meta(build_id, {
        "id": build_id, "session_id": session_id, "name": name,
        "code": code, "status": "queued", "attempt": 0,
        "created": time.time(), "error": None,
    })
    _q.put(build_id)
    return build_id
