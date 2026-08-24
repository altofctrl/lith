"""Manual moderation CLI for the knappery (no automated review in this MVP).

Usage:
  ./venv/bin/python admin.py reports              list open reports, newest first
  ./venv/bin/python admin.py hide <listing_id>     hide a listing (soft delete)
  ./venv/bin/python admin.py hide-comment <id>     hide a comment
  ./venv/bin/python admin.py show <listing_id>     listing detail + its comments
"""

import sqlite3
import sys
import time

import knappery


def _conn():
    return knappery._conn()


def cmd_reports():
    conn = _conn()
    rows = conn.execute("""
        SELECT r.created_at, r.reason, r.listing_id, r.comment_id, l.title, l.author
        FROM reports r JOIN listings l ON l.id = r.listing_id
        ORDER BY r.created_at DESC LIMIT 100
    """).fetchall()
    conn.close()
    if not rows:
        print("no reports")
        return
    for r in rows:
        when = time.strftime("%Y-%m-%d %H:%M", time.localtime(r["created_at"]))
        target = f"comment {r['comment_id']}" if r["comment_id"] else "listing"
        print(f"{when}  [{target}]  \"{r['title']}\" by {r['author']}  ({r['listing_id']})")
        if r["reason"]:
            print(f"           reason: {r['reason']}")


def cmd_hide(listing_id):
    conn = _conn()
    conn.execute("UPDATE listings SET hidden = 1 WHERE id = ?", (listing_id,))
    conn.commit()
    n = conn.total_changes
    conn.close()
    print("hidden" if n else "not found")


def cmd_hide_comment(comment_id):
    conn = _conn()
    conn.execute("UPDATE comments SET hidden = 1 WHERE id = ?", (comment_id,))
    conn.commit()
    n = conn.total_changes
    conn.close()
    print("hidden" if n else "not found")


def cmd_show(listing_id):
    conn = _conn()
    row = conn.execute("SELECT * FROM listings WHERE id = ?", (listing_id,)).fetchone()
    if not row:
        print("not found")
        return
    print(dict(row))
    for c in conn.execute("SELECT * FROM comments WHERE listing_id = ? ORDER BY created_at", (listing_id,)):
        print(" ", dict(c))
    conn.close()


if __name__ == "__main__":
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        sys.exit(0)
    cmd, rest = args[0], args[1:]
    if cmd == "reports":
        cmd_reports()
    elif cmd == "hide" and rest:
        cmd_hide(rest[0])
    elif cmd == "hide-comment" and rest:
        cmd_hide_comment(rest[0])
    elif cmd == "show" and rest:
        cmd_show(rest[0])
    else:
        print(__doc__)
