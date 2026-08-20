"""lith knapp backend: Oldowan chat + firmware build + flash artifacts.

Runs on 127.0.0.1:3130, proxied by nginx at lith.vidalion.co/api/.

Endpoints
  GET  /api/health                    service + toolchain + provider status
  POST /api/session                   -> {session_id, envelope}   (canned greeting, no LLM call)
  POST /api/chat {session_id, message}-> {envelope, build_id?}
  GET  /api/build/<id>                -> {status, attempt, name, say?, error_public?}
  GET  /api/build/<id>/manifest.json  esp-web-tools manifest
  GET  /api/build/<id>/<file>.bin     flash binaries
  POST /api/stock/restore {session_id} -> queue a build of the real
       shipped firmware (stock_pomodoro.ino), bypassing Oldowan

  the knappery (community listings, see knappery.py)
  POST   /api/knappery/publish                 {build_id, session_id, title, author, blurb}
  GET    /api/knappery?sort=stars|new          browse
  GET    /api/knappery/<id>                    detail + comments
  DELETE /api/knappery/<id>                    {edit_token}
  POST   /api/knappery/<id>/star               {voter_id}  (toggles)
  GET    /api/knappery/<id>/comments
  POST   /api/knappery/<id>/comments           {author, body}
  POST   /api/knappery/<id>/report             {comment_id?, reason}
"""

import json
import logging
import logging.handlers
import os
import threading
import time
import uuid

from flask import Flask, jsonify, request, send_from_directory

import builder
import knappery
import oldowan

BASE = os.path.dirname(os.path.abspath(__file__))


def _setup_logging():
    """Log to logs/knapp.log as well as stderr.

    run.sh execs python with no redirect and the screen session has no -L, so
    for a long time the only copy of a traceback lived in scrollback and died
    with the next restart. Anything worth diagnosing has to outlive a restart.
    """
    logs = os.path.join(BASE, "logs")
    os.makedirs(logs, exist_ok=True)
    fmt = logging.Formatter(
        "%(asctime)s %(levelname)-7s %(name)s: %(message)s", "%Y-%m-%d %H:%M:%S")

    handlers = [logging.StreamHandler()]
    fh = logging.handlers.RotatingFileHandler(
        os.path.join(logs, "knapp.log"), maxBytes=5_000_000, backupCount=5,
        encoding="utf-8")
    handlers.append(fh)

    root = logging.getLogger()
    root.setLevel(logging.INFO)
    for h in handlers:
        h.setFormatter(fmt)
        root.addHandler(h)
    return os.path.join(logs, "knapp.log")


LOG_PATH = _setup_logging()

app = Flask(__name__)
app.logger.setLevel(logging.INFO)
logging.getLogger("werkzeug").setLevel(logging.INFO)

STOCK_NAME = "stock pomodoro timer"
with open(os.path.join(os.path.dirname(__file__), "stock_pomodoro.ino"), encoding="utf-8") as _f:
    STOCK_CODE = _f.read()

MAX_MESSAGE_CHARS = 4000
RATE = {
    "chat": (30, 3600), "build": (8, 3600),
    "publish": (10, 3600), "star": (120, 3600),
    "comment": (30, 3600), "report": (20, 3600),
}  # per ip: (count, window s)
_hits = {}


def _client_ip():
    return request.headers.get("X-Real-IP") or request.remote_addr or "?"


def _rate_ok(kind):
    limit, window = RATE[kind]
    key = (kind, _client_ip())
    now = time.time()
    hits = [t for t in _hits.get(key, []) if now - t < window]
    if len(hits) >= limit:
        _hits[key] = hits
        return False
    hits.append(now)
    _hits[key] = hits
    return True


def _public_envelope(env, build_id=None):
    out = {"say": env["say"], "ask": env.get("ask"), "done": env.get("done", False)}
    if build_id:
        out["build_id"] = build_id
        out["name"] = env["build"]["name"]
    return out


def _norm(s):
    return " ".join(str(s or "").split()).casefold()


def _last_ask(sess):
    """The `ask` block from Oldowan's most recent turn, or None. Used both to
    recognise option clicks and to re-offer the question if a turn is
    rejected."""
    for m in reversed(sess["messages"]):
        if m["role"] != "assistant":
            continue
        try:
            return (json.loads(m["content"]) or {}).get("ask")
        except ValueError:
            return None
    return None


ADVERSARIAL_LOG = os.path.join(os.path.dirname(__file__), "adversarial_log.txt")


def _log_adversarial(session_id, text):
    entry = f"{time.strftime('%Y-%m-%d %H:%M:%S')} | {session_id} | {text}\n"
    with open(ADVERSARIAL_LOG, "a", encoding="utf-8") as f:
        f.write(entry)


@app.get("/api/health")
def health():
    prov = oldowan.load_provider()
    return jsonify({
        "ok": True,
        "provider": prov["_name"],
        "toolchain": builder.toolchain_ready(),
    })


@app.post("/api/session")
def new_session():
    if not _rate_ok("chat"):
        return jsonify({"error": "rate limited"}), 429
    session_id = uuid.uuid4().hex
    greeting = oldowan.GREETING
    builder.write_session(session_id, {
        "id": session_id,
        "created": time.time(),
        "messages": [{"role": "assistant", "hidden": False,
                      "content": json.dumps(greeting)}],
        "builds": [],
    })
    return jsonify({"session_id": session_id,
                    "envelope": _public_envelope(greeting)})


@app.get("/api/session/<session_id>")
def get_session(session_id):
    """Visible transcript replay, so a session can be reopened (or moved
    phone -> laptop via the #s= link) without losing the conversation."""
    sess = builder.read_session(session_id)
    if sess is None:
        return jsonify({"error": "unknown session"}), 404
    items = []
    build_i = 0
    for m in sess["messages"]:
        if m.get("hidden"):
            continue
        if m["role"] == "user":
            items.append({"role": "user", "text": m["content"]})
            continue
        try:
            env = json.loads(m["content"])
        except ValueError:
            continue
        item = {"role": "oldowan", "say": env.get("say", ""),
                "ask": env.get("ask"), "done": env.get("done", False)}
        if env.get("build") and build_i < len(sess["builds"]):
            item["build_id"] = sess["builds"][build_i]
            item["name"] = env["build"].get("name", "knapp")
            build_i += 1
        items.append(item)
    return jsonify({"session_id": session_id, "items": items})


@app.post("/api/chat")
def chat():
    if not _rate_ok("chat"):
        return jsonify({"error": "rate limited"}), 429
    data = request.get_json(silent=True) or {}
    session_id = str(data.get("session_id", ""))
    message = str(data.get("message", "")).strip()
    if not message:
        return jsonify({"error": "empty message"}), 400
    if len(message) > MAX_MESSAGE_CHARS:
        return jsonify({"error": "message too long"}), 400
    sess = builder.read_session(session_id)
    if sess is None:
        return jsonify({"error": "unknown session"}), 404

    # Clicking an option sends Oldowan's own words back, so there is nothing to
    # classify. Skipping the guard there is not just a saving: the guard sees a
    # bare message with no conversation context, and on 2026-08-13 it flagged
    # the option "headcount x average salary, encoder adjusts headcount"
    # verbatim from its own question, silently killing that session.
    prev_ask = _last_ask(sess)
    offered = {_norm(o) for o in ((prev_ask or {}).get("options") or [])}

    if _norm(message) not in offered and oldowan.is_adversarial(message):
        _log_adversarial(session_id, message)
        # Tell the user something came back, and re-offer the question they
        # were answering so the buttons return. Nothing is written to the
        # transcript, so a false positive costs a rephrase rather than the
        # whole session.
        return jsonify(_public_envelope({
            "say": "sorry, that didn't come through on my end. mind putting it "
                   "another way?",
            "ask": prev_ask,
            "done": False,
        }))

    sess["messages"].append({"role": "user", "hidden": False, "content": message})
    try:
        env = oldowan.chat(sess["messages"])
    except oldowan.Truncated as e:
        # Distinct from a dead provider: the model answered, it just ran out of
        # room mid-sketch. Rolling the user turn back leaves the session intact
        # so they can retry without starting over.
        sess["messages"].pop()
        builder.write_session(session_id, sess)
        app.logger.error("session %s: truncated turn: %s", session_id, e)
        return jsonify({"error": "that knapp came out too big to finish in one "
                                 "pass. try asking for it again, or trim a "
                                 "feature and i'll shape a smaller one.",
                        "detail": str(e)[:200]}), 502
    except Exception as e:
        sess["messages"].pop()
        builder.write_session(session_id, sess)
        app.logger.exception("session %s: provider call failed", session_id)
        return jsonify({"error": "oldowan is unreachable right now",
                        "detail": str(e)[:200]}), 502

    sess["messages"].append({"role": "assistant", "hidden": False,
                             "content": json.dumps(env)})

    build_id = None
    if env.get("build"):
        if not _rate_ok("build"):
            builder.write_session(session_id, sess)
            return jsonify({"error": "build rate limited, try again later"}), 429
        if not builder.toolchain_ready():
            builder.write_session(session_id, sess)
            return jsonify({"error": "build toolchain unavailable"}), 503
        build_id = builder.enqueue(session_id, env["build"]["name"],
                                   env["build"]["code"])
        sess["builds"].append(build_id)
    builder.write_session(session_id, sess)
    return jsonify(_public_envelope(env, build_id))


@app.post("/api/stock/restore")
def stock_restore():
    """Reflash the exact firmware lith ships with, bypassing Oldowan
    entirely so it's always the real thing, not a regeneration."""
    if not _rate_ok("build"):
        return jsonify({"error": "build rate limited, try again later"}), 429
    if not builder.toolchain_ready():
        return jsonify({"error": "build toolchain unavailable"}), 503
    data = request.get_json(silent=True) or {}
    session_id = str(data.get("session_id", ""))
    sess = builder.read_session(session_id)
    if sess is None:
        return jsonify({"error": "unknown session"}), 404

    env = {
        "say": "no fuss. reflashing the exact firmware your lith shipped "
               "with, same pomodoro timer, same everything.",
        "ask": None,
        "build": {"name": STOCK_NAME, "code": STOCK_CODE},
        "done": True,
    }
    sess["messages"].append({"role": "user", "hidden": False,
                             "content": "restore the stock pomodoro timer"})
    sess["messages"].append({"role": "assistant", "hidden": False,
                             "content": json.dumps(env)})
    build_id = builder.enqueue(session_id, STOCK_NAME, STOCK_CODE)
    sess["builds"].append(build_id)
    builder.write_session(session_id, sess)
    return jsonify(_public_envelope(env, build_id))


@app.get("/api/build/<build_id>")
def build_status(build_id):
    meta = builder.read_meta(build_id)
    if meta is None:
        return jsonify({"error": "unknown build"}), 404
    out = {"id": meta["id"], "status": meta["status"],
           "attempt": meta.get("attempt", 0), "name": meta.get("name")}
    if meta["status"] == "failed":
        out["say"] = meta.get("say") or "that one fractured; let's try a different shape."
    return jsonify(out)


@app.get("/api/build/<build_id>/<path:filename>")
def build_file(build_id, filename):
    meta = builder.read_meta(build_id)
    if meta is None or meta["status"] != "ready":
        return jsonify({"error": "not ready"}), 404
    out_dir = os.path.join(builder.BUILDS, build_id, "out")
    return send_from_directory(out_dir, filename)


@app.get("/api/build/<build_id>/source")
def build_source(build_id):
    meta = builder.read_meta(build_id)
    if meta is None:
        return jsonify({"error": "unknown build"}), 404
    return app.response_class(meta.get("code", ""), mimetype="text/plain")


@app.post("/api/knappery/publish")
def knappery_publish():
    if not _rate_ok("publish"):
        return jsonify({"error": "rate limited"}), 429
    data = request.get_json(silent=True) or {}
    build_id = str(data.get("build_id", ""))
    session_id = str(data.get("session_id", ""))
    meta = builder.read_meta(build_id)
    if meta is None or meta.get("status") != "ready":
        return jsonify({"error": "build not ready"}), 400
    sess = builder.read_session(session_id)
    if sess is None or build_id not in sess.get("builds", []):
        return jsonify({"error": "build does not belong to this session"}), 403
    try:
        listing_id, edit_token = knappery.publish(
            build_id, data.get("title", ""), data.get("author", ""), data.get("blurb", ""))
    except knappery.ValidationError as e:
        return jsonify({"error": str(e)}), 400
    _review_async(listing_id, data.get("title", ""), data.get("blurb", ""), meta.get("code", ""))
    return jsonify({"listing_id": listing_id, "edit_token": edit_token})


def _review_async(listing_id, title, blurb, code):
    """Advisory automated review, off the request path. The listing is already
    live; a concerning verdict just files a report for `admin.py reports`, so
    publish latency and provider flakiness never affect the publisher."""
    def run():
        concerning, reason = oldowan.review_knapp(title, blurb, code)
        if concerning:
            knappery.report(listing_id, None, knappery.AUTO_REPORT_PREFIX + (reason or "flagged"))

    threading.Thread(target=run, daemon=True).start()


@app.get("/api/knappery")
def knappery_list():
    sort = "stars" if request.args.get("sort") != "new" else "new"
    try:
        limit = min(max(int(request.args.get("limit", 40)), 1), 100)
        offset = max(int(request.args.get("offset", 0)), 0)
    except ValueError:
        return jsonify({"error": "bad limit/offset"}), 400
    voter_id = request.args.get("voter") or None
    return jsonify({"listings": knappery.list_listings(sort, limit, offset, voter_id)})


@app.get("/api/knappery/<listing_id>")
def knappery_detail(listing_id):
    voter_id = request.args.get("voter") or None
    listing = knappery.get_listing(listing_id, voter_id)
    if listing is None:
        return jsonify({"error": "unknown listing"}), 404
    listing["comments_list"] = knappery.list_comments(listing_id)
    return jsonify(listing)


@app.delete("/api/knappery/<listing_id>")
def knappery_delete(listing_id):
    data = request.get_json(silent=True) or {}
    ok = knappery.delete_listing(listing_id, str(data.get("edit_token", "")))
    if not ok:
        return jsonify({"error": "not found or wrong token"}), 403
    return jsonify({"ok": True})


@app.post("/api/knappery/<listing_id>/star")
def knappery_star(listing_id):
    if not _rate_ok("star"):
        return jsonify({"error": "rate limited"}), 429
    data = request.get_json(silent=True) or {}
    voter_id = str(data.get("voter_id", "")).strip()
    if not voter_id:
        return jsonify({"error": "voter_id required"}), 400
    if knappery.get_listing(listing_id) is None:
        return jsonify({"error": "unknown listing"}), 404
    starred, count = knappery.toggle_star(listing_id, voter_id)
    return jsonify({"starred": starred, "stars": count})


@app.get("/api/knappery/<listing_id>/comments")
def knappery_comments(listing_id):
    if knappery.get_listing(listing_id) is None:
        return jsonify({"error": "unknown listing"}), 404
    return jsonify({"comments": knappery.list_comments(listing_id)})


@app.post("/api/knappery/<listing_id>/comments")
def knappery_add_comment(listing_id):
    if not _rate_ok("comment"):
        return jsonify({"error": "rate limited"}), 429
    data = request.get_json(silent=True) or {}
    try:
        comment = knappery.add_comment(listing_id, data.get("author", ""), data.get("body", ""))
    except knappery.ValidationError as e:
        return jsonify({"error": str(e)}), 400
    if comment is None:
        return jsonify({"error": "unknown listing"}), 404
    return jsonify(comment)


@app.post("/api/knappery/<listing_id>/report")
def knappery_report(listing_id):
    if not _rate_ok("report"):
        return jsonify({"error": "rate limited"}), 429
    data = request.get_json(silent=True) or {}
    ok = knappery.report(listing_id, data.get("comment_id"), data.get("reason", ""))
    if not ok:
        return jsonify({"error": "unknown listing"}), 404
    return jsonify({"ok": True})


builder.start_worker()

if __name__ == "__main__":
    app.run(host="127.0.0.1", port=3130, threaded=True)
