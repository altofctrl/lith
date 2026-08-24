"""the knappery: community listings for published knapps.

SQLite-backed. No accounts: publishers pick a display name and get back an
edit_token (stored client-side) that lets them delete their own listing later.
Stars are keyed by a client-generated voter_id (localStorage), not identity.
Listings go live immediately; a report table collects flags for Aaron to
moderate by hand (see admin.py). Reports come from two places: the report
button in the UI, and an advisory automated review of each published sketch
(app.py `_review_async` -> oldowan.review_knapp), whose reports are marked
with AUTO_REPORT_PREFIX. Neither one hides a listing on its own.
"""

import os
import re
import sqlite3
import time
import uuid

DB_PATH = os.path.join(os.path.dirname(__file__), "knappery.db")

TITLE_MAX = 80
AUTHOR_MAX = 40
BLURB_MAX = 400
COMMENT_MAX = 1000
REASON_MAX = 300

# marks a report filed by the automated review rather than by a visitor
AUTO_REPORT_PREFIX = "[auto] "


def _conn():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA foreign_keys = ON")
    return conn


def init_db():
    conn = _conn()
    conn.executescript("""
        CREATE TABLE IF NOT EXISTS listings (
            id TEXT PRIMARY KEY,
            build_id TEXT NOT NULL,
            title TEXT NOT NULL,
            author TEXT NOT NULL,
            blurb TEXT NOT NULL DEFAULT '',
            tags TEXT NOT NULL DEFAULT '',
            edit_token TEXT NOT NULL,
            created_at REAL NOT NULL,
            reported_count INTEGER NOT NULL DEFAULT 0,
            hidden INTEGER NOT NULL DEFAULT 0
        );
        CREATE TABLE IF NOT EXISTS stars (
            listing_id TEXT NOT NULL,
            voter_id TEXT NOT NULL,
            created_at REAL NOT NULL,
            PRIMARY KEY (listing_id, voter_id)
        );
        CREATE TABLE IF NOT EXISTS comments (
            id TEXT PRIMARY KEY,
            listing_id TEXT NOT NULL,
            author TEXT NOT NULL,
            body TEXT NOT NULL,
            created_at REAL NOT NULL,
            hidden INTEGER NOT NULL DEFAULT 0
        );
        CREATE TABLE IF NOT EXISTS reports (
            id TEXT PRIMARY KEY,
            listing_id TEXT NOT NULL,
            comment_id TEXT,
            reason TEXT NOT NULL DEFAULT '',
            created_at REAL NOT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_stars_listing ON stars(listing_id);
        CREATE INDEX IF NOT EXISTS idx_comments_listing ON comments(listing_id);
    """)
    existing_cols = {r["name"] for r in conn.execute("PRAGMA table_info(listings)").fetchall()}
    if "tags" not in existing_cols:
        conn.execute("ALTER TABLE listings ADD COLUMN tags TEXT NOT NULL DEFAULT ''")
    conn.commit()
    conn.close()


_WS = re.compile(r"\s+")


def _clean(s, max_len):
    s = _WS.sub(" ", str(s or "")).strip()
    return s[:max_len]


class ValidationError(Exception):
    pass


def publish(build_id, title, author, blurb, tags=""):
    """tags is server-set only (e.g. by admin/seed scripts). The public
    /api/knappery/publish route never forwards client input into it, so a
    visitor can't self-apply a label like "lith originals"."""
    title = _clean(title, TITLE_MAX)
    author = _clean(author, AUTHOR_MAX)
    blurb = _clean(blurb, BLURB_MAX)
    tags = _clean(tags, 200)
    if not title:
        raise ValidationError("title required")
    if not author:
        raise ValidationError("author required")
    listing_id = uuid.uuid4().hex
    edit_token = uuid.uuid4().hex
    conn = _conn()
    conn.execute(
        "INSERT INTO listings (id, build_id, title, author, blurb, tags, edit_token, created_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        (listing_id, build_id, title, author, blurb, tags, edit_token, time.time()),
    )
    conn.commit()
    conn.close()
    return listing_id, edit_token


def _row_to_listing(row, star_count, comment_count, starred):
    tags = row["tags"] if "tags" in row.keys() else ""
    return {
        "id": row["id"],
        "build_id": row["build_id"],
        "title": row["title"],
        "author": row["author"],
        "blurb": row["blurb"],
        "tags": [t for t in tags.split(",") if t],
        "created_at": row["created_at"],
        "stars": star_count,
        "comments": comment_count,
        "starred": starred,
    }


def list_listings(sort="stars", limit=40, offset=0, voter_id=None):
    conn = _conn()
    order = "stars DESC, l.created_at DESC" if sort == "stars" else "l.created_at DESC"
    rows = conn.execute(f"""
        SELECT l.*,
               (SELECT COUNT(*) FROM stars s WHERE s.listing_id = l.id) AS stars,
               (SELECT COUNT(*) FROM comments c WHERE c.listing_id = l.id AND c.hidden = 0) AS comment_count
        FROM listings l
        WHERE l.hidden = 0
        ORDER BY {order}
        LIMIT ? OFFSET ?
    """, (limit, offset)).fetchall()
    starred_ids = set()
    if voter_id:
        starred_ids = {r["listing_id"] for r in conn.execute(
            "SELECT listing_id FROM stars WHERE voter_id = ?", (voter_id,)
        ).fetchall()}
    conn.close()
    return [_row_to_listing(r, r["stars"], r["comment_count"], r["id"] in starred_ids)
            for r in rows]


def get_listing(listing_id, voter_id=None):
    conn = _conn()
    row = conn.execute("SELECT * FROM listings WHERE id = ? AND hidden = 0", (listing_id,)).fetchone()
    if row is None:
        conn.close()
        return None
    stars = conn.execute("SELECT COUNT(*) c FROM stars WHERE listing_id = ?", (listing_id,)).fetchone()["c"]
    comment_count = conn.execute(
        "SELECT COUNT(*) c FROM comments WHERE listing_id = ? AND hidden = 0", (listing_id,)
    ).fetchone()["c"]
    starred = False
    if voter_id:
        starred = conn.execute(
            "SELECT 1 FROM stars WHERE listing_id = ? AND voter_id = ?", (listing_id, voter_id)
        ).fetchone() is not None
    conn.close()
    return _row_to_listing(row, stars, comment_count, starred)


def delete_listing(listing_id, edit_token):
    conn = _conn()
    row = conn.execute("SELECT edit_token FROM listings WHERE id = ?", (listing_id,)).fetchone()
    if row is None or row["edit_token"] != edit_token:
        conn.close()
        return False
    conn.execute("UPDATE listings SET hidden = 1 WHERE id = ?", (listing_id,))
    conn.commit()
    conn.close()
    return True


def toggle_star(listing_id, voter_id):
    """Returns (starred: bool, count: int) after toggling."""
    conn = _conn()
    exists = conn.execute(
        "SELECT 1 FROM stars WHERE listing_id = ? AND voter_id = ?", (listing_id, voter_id)
    ).fetchone()
    if exists:
        conn.execute("DELETE FROM stars WHERE listing_id = ? AND voter_id = ?", (listing_id, voter_id))
        starred = False
    else:
        conn.execute(
            "INSERT OR IGNORE INTO stars (listing_id, voter_id, created_at) VALUES (?, ?, ?)",
            (listing_id, voter_id, time.time()),
        )
        starred = True
    count = conn.execute("SELECT COUNT(*) c FROM stars WHERE listing_id = ?", (listing_id,)).fetchone()["c"]
    conn.commit()
    conn.close()
    return starred, count


def list_comments(listing_id, limit=200):
    conn = _conn()
    rows = conn.execute(
        "SELECT id, author, body, created_at FROM comments "
        "WHERE listing_id = ? AND hidden = 0 ORDER BY created_at ASC LIMIT ?",
        (listing_id, limit),
    ).fetchall()
    conn.close()
    return [dict(r) for r in rows]


def add_comment(listing_id, author, body):
    author = _clean(author, AUTHOR_MAX)
    body = _clean(body, COMMENT_MAX)
    if not body:
        raise ValidationError("comment required")
    if not author:
        author = "anon knapper"
    conn = _conn()
    exists = conn.execute("SELECT 1 FROM listings WHERE id = ? AND hidden = 0", (listing_id,)).fetchone()
    if not exists:
        conn.close()
        return None
    comment_id = uuid.uuid4().hex
    conn.execute(
        "INSERT INTO comments (id, listing_id, author, body, created_at) VALUES (?, ?, ?, ?, ?)",
        (comment_id, listing_id, author, body, time.time()),
    )
    conn.commit()
    conn.close()
    return {"id": comment_id, "author": author, "body": body, "created_at": time.time()}


def report(listing_id, comment_id, reason):
    conn = _conn()
    exists = conn.execute("SELECT 1 FROM listings WHERE id = ?", (listing_id,)).fetchone()
    if not exists:
        conn.close()
        return False
    reason = _clean(reason, REASON_MAX)
    conn.execute(
        "INSERT INTO reports (id, listing_id, comment_id, reason, created_at) VALUES (?, ?, ?, ?, ?)",
        (uuid.uuid4().hex, listing_id, comment_id, reason, time.time()),
    )
    conn.execute("UPDATE listings SET reported_count = reported_count + 1 WHERE id = ?", (listing_id,))
    conn.commit()
    conn.close()
    return True


init_db()
