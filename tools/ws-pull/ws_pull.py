#!/usr/bin/python3
"""ws-pull: copies the weather station's readings from the display to this computer.

Run every hour by ws-pull.timer (see README.md). Each run:
  1. asks the display for the 15-minute slots changed since the last run:
     GET /api/readings/history?fmt=json&cursor=<the cursor kept from the last run>
  2. checks every field of the answer,
  3. merges the slots into one CSV file per month (UTC) in <state_dir>/archive/,
     one row per slot: a slot that comes again (a late outdoor reading, or a
     display restart sending everything once more) replaces its row,
  4. only then keeps the display's new cursor.

The display is treated as untrusted. The answer is limited in size and time,
every value must be a number, true/false or null, field names are checked, and
nothing from it reaches a shell or a file name (month files are named from
checked timestamps). Any doubt rejects the whole answer: nothing is written and
the cursor stays, so the next run asks again. Worst case: junk numbers in the
archive.

Fields the display adds later (more sensors) become new columns by themselves.

Standard library only; Python 3.10 or newer.
"""

import argparse
import base64
import configparser
import csv
import datetime
import http.client
import io
import json
import math
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

try:
    import fcntl
except ImportError:  # not Linux (the tests also run elsewhere)
    fcntl = None

VERSION = "1"
DEFAULT_CONFIG = "/etc/ws-pull/ws-pull.conf"

# The archive's columns: these first, in this order (the display's own CSV
# order, plus p_sea after the pressure). Anything else the display sends
# follows, in the order it first appears.
BASE_COLUMNS = ["ts", "time", "sht_t", "sht_rh", "bme_t", "bme_rh", "bme_p", "p_sea",
                "bat_mv", "rssi", "readings", "late", "low_battery", "in_t", "in_rh"]
MADE_HERE = {"time", "p_sea"}                       # if the display sends these: ignored
NAME_RE = re.compile(r"[a-z][a-z0-9_]{0,31}")
CURSOR_RE = re.compile(r"[0-9a-f]{8}-[0-9]{1,10}")  # readings_store_cursor(): boot id, change number
MAX_FIELDS = 64                                     # per slot
TS_MIN = 1735689600                                 # 2025-01-01: older is a clock that wasn't set
TS_AHEAD = 2 * 86400                                # further ahead than this: a wrong clock too
VALUE_MAX = 1e15


class Refused(Exception):
    """The answer can't be used: nothing is written, the cursor stays."""


class Unreachable(Exception):
    """No complete answer: worth another try."""


class ConfigError(Exception):
    pass


def say(text):
    print(text, flush=True)


def warn(text):
    print(f"warning: {text}", file=sys.stderr, flush=True)


def iso(ts):
    return datetime.datetime.fromtimestamp(ts, datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def month_of(ts):
    return datetime.datetime.fromtimestamp(ts, datetime.timezone.utc).strftime("%Y-%m")


def running_as_root():
    return hasattr(os, "geteuid") and os.geteuid() == 0


# --- settings ----------------------------------------------------------------

def _int(section, key, default, lo, hi, path):
    raw = section.get(key)
    if raw is None:
        return default
    try:
        value = int(raw.strip())
    except ValueError:
        raise ConfigError(f"{path}: {key} must be a whole number") from None
    if not lo <= value <= hi:
        raise ConfigError(f"{path}: {key} must be between {lo} and {hi}")
    return value


class Config:
    def __init__(self, path):
        parser = configparser.ConfigParser(interpolation=None)
        try:
            with open(path, encoding="utf-8") as f:
                parser.read_file(f)
        except OSError as e:
            raise ConfigError(f"can't read {path}: {e.strerror}") from None
        except configparser.Error as e:
            raise ConfigError(f"{path}: {e}") from None
        if not parser.has_section("ws-pull"):
            raise ConfigError(f"{path}: no [ws-pull] section")
        s = parser["ws-pull"]
        self.url = s.get("url", "").strip().rstrip("/")
        parts = urllib.parse.urlsplit(self.url)
        if parts.scheme not in ("http", "https") or not parts.netloc or parts.query or parts.fragment:
            raise ConfigError(f"{path}: url must look like http://192.168.1.50 or http://esp32flight.local")
        self.user = s.get("user", "admin").strip()
        self.password_file = s.get("password_file", "/etc/ws-pull/password").strip()
        self.state_dir = s.get("state_dir", "/var/lib/ws-pull").strip()
        self.timeout_s = _int(s, "timeout_s", 60, 5, 600, path)
        self.max_bytes = _int(s, "max_bytes", 8 * 1024 * 1024, 1024, 256 * 1024 * 1024, path)
        self.attempts = _int(s, "attempts", 3, 1, 10, path)
        self.retry_wait_s = _int(s, "retry_wait_s", 30, 0, 600, path)


def read_password(cfg):
    """systemd hands the password over with LoadCredential=; otherwise the file itself."""
    creds = os.environ.get("CREDENTIALS_DIRECTORY")
    path = os.path.join(creds, "password") if creds else cfg.password_file
    try:
        with open(path, encoding="utf-8") as f:
            return f.read().rstrip("\r\n")
    except FileNotFoundError:
        return ""
    except OSError as e:
        raise ConfigError(f"can't read the password ({path}: {e.strerror}); "
                          "run it as the service: sudo systemctl start ws-pull") from None


# --- files -------------------------------------------------------------------

def write_atomic(path, text):
    """A crash or power cut leaves either the old file or the new one, never half of it."""
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="") as f:
        f.write(text)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def sync_dir(path):
    """Makes the renames above last through a power cut."""
    if os.name != "posix":
        return
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def take_lock(state_dir):
    """None: no locking here; False: another run holds it."""
    if fcntl is None:
        return None
    f = open(os.path.join(state_dir, "lock"), "a")
    try:
        fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        f.close()
        return False
    return f


def read_cursor(path):
    try:
        with open(path, encoding="utf-8") as f:
            text = f.read().strip()
    except FileNotFoundError:
        return ""
    if text and not CURSOR_RE.fullmatch(text):
        warn("the saved cursor looks wrong; asking the display for everything")
        return ""
    return text


# --- the display -------------------------------------------------------------

class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None  # the 3xx then comes back as an HTTPError


def _reason(e):
    return str(getattr(e, "reason", None) or e)[:200]


def fetch(cfg, cursor, password):
    """-> (X-Cursor header, body bytes)"""
    query = {"fmt": "json"}
    if cursor:
        query["cursor"] = cursor
    req = urllib.request.Request(f"{cfg.url}/api/readings/history?{urllib.parse.urlencode(query)}",
                                 headers={"Accept": "application/json", "User-Agent": f"ws-pull/{VERSION}"})
    if password:
        cred = base64.b64encode(f"{cfg.user}:{password}".encode("utf-8")).decode("ascii")
        req.add_header("Authorization", f"Basic {cred}")
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), _NoRedirect())
    deadline = time.monotonic() + cfg.timeout_s
    try:
        resp = opener.open(req, timeout=min(15, cfg.timeout_s))
    except urllib.error.HTTPError as e:
        e.close()
        if e.code == 401:
            raise Refused("the display wants the panel password (HTTP 401): check /etc/ws-pull/password") from None
        raise Refused(f"the display answered HTTP {e.code}") from None
    except (urllib.error.URLError, http.client.HTTPException, OSError) as e:
        raise Unreachable(f"no answer from {cfg.url}: {_reason(e)}") from None
    with resp:
        if resp.status != 200:
            raise Refused(f"the display answered HTTP {resp.status}")
        if not resp.headers.get("Content-Type", "").startswith("application/json"):
            raise Refused("the answer isn't JSON (Content-Type)")
        length = resp.headers.get("Content-Length", "")
        if length.isdigit() and int(length) > cfg.max_bytes:
            raise Refused(f"the answer is bigger than {cfg.max_bytes} bytes (max_bytes)")
        chunks, size = [], 0
        try:
            while True:
                if time.monotonic() > deadline:
                    raise Unreachable(f"the answer took longer than {cfg.timeout_s} s")
                chunk = resp.read(65536)
                if not chunk:
                    break
                size += len(chunk)
                if size > cfg.max_bytes:
                    raise Refused(f"the answer is bigger than {cfg.max_bytes} bytes (max_bytes)")
                chunks.append(chunk)
        except (http.client.HTTPException, OSError) as e:
            raise Unreachable(f"the answer broke off: {_reason(e)}") from None
        return resp.headers.get("X-Cursor", ""), b"".join(chunks)


def fetch_retrying(cfg, cursor, password):
    for attempt in range(1, cfg.attempts + 1):
        try:
            return fetch(cfg, cursor, password)
        except Unreachable as e:
            if attempt == cfg.attempts:
                raise
            say(f"try {attempt} of {cfg.attempts}: {e}; again in {cfg.retry_wait_s} s")
            time.sleep(cfg.retry_wait_s)


def _reject_constant(name):
    raise ValueError(f"{name} isn't a number")


def cell(name, value):
    """One value as CSV text: numbers as they are, true/false as 1/0, null as empty."""
    if value is None:
        return ""
    if isinstance(value, bool):
        return "1" if value else "0"
    if isinstance(value, int):
        if abs(value) > VALUE_MAX:
            raise Refused(f"{name}: number out of range")
        return str(value)
    if isinstance(value, float):
        if not math.isfinite(value) or abs(value) > VALUE_MAX:
            raise Refused(f"{name}: number out of range")
        text = repr(value)
        if "e" in text or "E" in text:  # no exponents in the CSV
            text = f"{value:.10f}".rstrip("0").rstrip(".")
        return text
    raise Refused(f"{name}: not a number, true/false or null")


def parse(body, header_cursor, now):
    """-> (new cursor, rows by ts, slots skipped, names ignored)"""
    try:
        doc = json.loads(body.decode("utf-8"), parse_constant=_reject_constant)
    except (ValueError, RecursionError) as e:
        raise Refused(f"the answer isn't valid JSON: {str(e)[:200]}") from None
    if not isinstance(doc, dict):
        raise Refused("the answer isn't a JSON object")
    cursor = doc.get("cursor")
    if not isinstance(cursor, str) or not CURSOR_RE.fullmatch(cursor):
        raise Refused("the answer has no valid cursor")
    if header_cursor and header_cursor != cursor:
        raise Refused("X-Cursor and the answer's cursor differ")
    at_sea = doc.get("pressure_at_sea_level")
    if not isinstance(at_sea, bool):
        raise Refused("pressure_at_sea_level is missing or not true/false")
    slots = doc.get("slots")
    if not isinstance(slots, list):
        raise Refused("the answer has no list of slots")
    rows, skipped, ignored = {}, 0, set()
    for slot in slots:
        if not isinstance(slot, dict):
            raise Refused("a slot isn't a JSON object")
        if len(slot) > MAX_FIELDS:
            raise Refused(f"a slot has more than {MAX_FIELDS} fields")
        ts = slot.get("ts")
        if type(ts) is not int:
            raise Refused("a slot without a whole-number ts")
        if not TS_MIN <= ts <= now + TS_AHEAD:
            skipped += 1
            continue
        row = {"ts": str(ts), "time": iso(ts)}
        for name, value in slot.items():
            if name == "ts":
                continue
            if not NAME_RE.fullmatch(name):
                raise Refused(f"a field name that isn't allowed: {name[:40]!r}")
            if name in MADE_HERE:
                ignored.add(name)
                continue
            row[name] = cell(name, value)
        if row.get("bme_p"):
            row["p_sea"] = "1" if at_sea else "0"
        rows[ts] = row  # a slot twice in one answer: the later one counts
    return cursor, rows, skipped, sorted(ignored)


# --- the archive -------------------------------------------------------------

def read_month(path):
    """-> (header, rows by ts, damaged). A file that can't be read is moved aside."""
    try:
        f = open(path, encoding="utf-8", newline="")
    except FileNotFoundError:
        return list(BASE_COLUMNS), {}, False
    try:
        with f:
            reader = csv.reader(f)
            head = next(reader, None)
            if (head is None or head[:2] != ["ts", "time"] or len(set(head)) != len(head)
                    or not all(NAME_RE.fullmatch(h) for h in head)):
                raise ValueError("unexpected header")
            rows = {}
            for rec in reader:
                if len(rec) != len(head):
                    raise ValueError(f"line {reader.line_num} has {len(rec)} cells, not {len(head)}")
                row = dict(zip(head, rec))
                rows[int(row["ts"])] = row
    except (ValueError, csv.Error) as e:
        aside = f"{path}.damaged-{time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())}"
        os.replace(path, aside)
        warn(f"{os.path.basename(path)} couldn't be read ({str(e)[:200]}): moved to {os.path.basename(aside)}; "
             "the next run asks the display for everything it has")
        return list(BASE_COLUMNS), {}, True
    header = list(BASE_COLUMNS) + [h for h in head if h not in BASE_COLUMNS]
    return header, rows, False


def write_month(path, header, rows):
    buf = io.StringIO()
    writer = csv.writer(buf, lineterminator="\n")
    writer.writerow(header)
    for ts in sorted(rows):
        writer.writerow([rows[ts].get(h, "") for h in header])
    write_atomic(path, buf.getvalue())


def merge(archive_dir, rows):
    """-> (month names written, counts, a damaged file was found)"""
    months = {}
    for ts, row in rows.items():
        months.setdefault(month_of(ts), {})[ts] = row
    counts = {"new": 0, "changed": 0, "same": 0}
    damaged = False
    for month in sorted(months):
        path = os.path.join(archive_dir, f"{month}.csv")
        header, old, was_damaged = read_month(path)
        damaged = damaged or was_damaged
        for ts, row in sorted(months[month].items()):
            for name in row:
                if name not in header:
                    header.append(name)
            prev = old.get(ts)
            if prev is None:
                counts["new"] += 1
            elif all(prev.get(h, "") == row.get(h, "") for h in header):
                counts["same"] += 1
            else:
                counts["changed"] += 1
            old[ts] = row
        write_month(path, header, old)
    return sorted(months), counts, damaged


def write_status(archive_dir, ok, message, newest):
    """status.json: for a look by hand and the homelab's check."""
    path = os.path.join(archive_dir, "status.json")
    try:
        with open(path, encoding="utf-8") as f:
            old = json.load(f)
        if not isinstance(old, dict):
            old = {}
    except (OSError, ValueError):
        old = {}
    now = iso(int(time.time()))
    known = [t for t in (old.get("newest_slot"), newest) if isinstance(t, str)]
    status = {
        "version": VERSION,
        "last_try": now,
        "ok": ok,
        "message": message,
        "last_success": now if ok else old.get("last_success"),
        "newest_slot": max(known) if known else None,
    }
    try:
        write_atomic(path, json.dumps(status, indent=1) + "\n")
    except OSError as e:
        warn(f"status.json not written: {e.strerror}")


# --- one run -----------------------------------------------------------------

def run(cfg, password, archive):
    cursor_path = os.path.join(cfg.state_dir, "cursor")
    cursor = read_cursor(cursor_path)
    try:
        header_cursor, body = fetch_retrying(cfg, cursor, password)
        new_cursor, rows, skipped, ignored = parse(body, header_cursor, int(time.time()))
        months, counts, damaged = merge(archive, rows)
        sync_dir(archive)
        write_atomic(cursor_path, ("" if damaged else new_cursor) + "\n")
        sync_dir(cfg.state_dir)
    except (Refused, Unreachable) as e:
        write_status(archive, False, str(e), None)
        print(f"failed: {e}", file=sys.stderr, flush=True)
        return 1
    except OSError as e:
        write_status(archive, False, f"writing: {e}", None)
        print(f"failed writing: {e}", file=sys.stderr, flush=True)
        return 1

    if rows:
        newest, n = iso(max(rows)), len(rows)
        message = (f"{n} slot{'' if n == 1 else 's'}: {counts['new']} new, {counts['changed']} changed, "
                   f"{counts['same']} the same ({', '.join(m + '.csv' for m in months)}); newest {newest}")
    else:
        newest = None
        message = "nothing new"
    write_status(archive, True, message, newest)
    say(message)
    if cursor and new_cursor[:8] != cursor[:8]:
        say("the display has restarted since the last run, so it sent everything once more")
    if skipped:
        warn(f"{skipped} slot{'' if skipped == 1 else 's'} skipped: timestamp before 2025 or more than 2 days ahead (the display's clock?)")
    for name in ignored:
        warn(f"the display sends a field '{name}', which ws-pull makes itself: ignored")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="Copies the weather station's readings from the display "
                                             "into monthly CSV files (see README.md).")
    ap.add_argument("--config", default=DEFAULT_CONFIG, help=f"settings file (default {DEFAULT_CONFIG})")
    args = ap.parse_args(argv)
    if running_as_root():
        print("ws-pull: not as root (its files would then belong to root and the service couldn't write them); "
              "run it as the service: sudo systemctl start ws-pull", file=sys.stderr)
        return 2
    try:
        cfg = Config(args.config)
        password = read_password(cfg)
    except ConfigError as e:
        print(f"ws-pull: {e}", file=sys.stderr)
        return 2
    archive = os.path.join(cfg.state_dir, "archive")
    try:
        os.makedirs(archive, mode=0o750, exist_ok=True)
        lock = take_lock(cfg.state_dir)
    except OSError as e:
        print(f"ws-pull: {cfg.state_dir}: {e.strerror}", file=sys.stderr)
        return 2
    if lock is False:
        say("another run is still going: skipped")
        return 0
    try:
        return run(cfg, password, archive)
    finally:
        if lock:
            lock.close()


if __name__ == "__main__":
    sys.exit(main())
