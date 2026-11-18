"""Tests for ws_pull.py against a fake display on 127.0.0.1.

    python3 -I -B test_ws_pull.py

Needs nothing installed and writes only to a temporary folder.
"""
import base64
import calendar
import contextlib
import csv
import io
import json
import os
import shutil
import socket
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ws_pull  # noqa: E402


def utc(year, month, day, hour=0, minute=0):
    return calendar.timegm((year, month, day, hour, minute, 0))


OCT_LAST = utc(2026, 10, 31, 23, 45)
NOV_FIRST = utc(2026, 11, 1, 0, 0)
NOV_LATER = utc(2026, 11, 1, 0, 15)


def slot(ts, **extra):
    s = {"ts": ts, "sht_t": 5.23, "sht_rh": 81.4, "bme_t": 5.61, "bme_rh": 78.2, "bme_p": 1013.4,
         "bat_mv": 3912, "rssi": -61, "readings": 1, "late": False, "low_battery": False,
         "in_t": 21.4, "in_rh": 44.1}
    s.update(extra)
    return s


class Fake:
    """What the fake display answers next."""
    status = 200
    ctype = "application/json"
    body = b""
    xcursor = ""
    password = ""
    requests = []


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        Fake.requests.append((self.path, self.headers.get("Authorization")))
        if Fake.password:
            want = "Basic " + base64.b64encode(f"admin:{Fake.password}".encode()).decode()
            if self.headers.get("Authorization") != want:
                self.send_response(401)
                self.send_header("WWW-Authenticate", 'Basic realm="esp32flight"')
                self.end_headers()
                self.wfile.write(b"auth required")
                return
        self.send_response(Fake.status)
        self.send_header("Content-Type", Fake.ctype)
        if Fake.xcursor:
            self.send_header("X-Cursor", Fake.xcursor)
        self.send_header("Content-Length", str(len(Fake.body)))
        self.end_headers()
        self.wfile.write(Fake.body)

    def log_message(self, *args):
        pass


def answer(slots, cursor="0badcafe-10", at_sea=True, xcursor=None):
    Fake.status, Fake.ctype = 200, "application/json"
    Fake.body = json.dumps({"cursor": cursor, "calibrated": True, "pressure_at_sea_level": at_sea,
                            "slots": slots}).encode()
    Fake.xcursor = cursor if xcursor is None else xcursor


class PullTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        cls.url = f"http://127.0.0.1:{cls.server.server_address[1]}"
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()

    def setUp(self):
        ws_pull.running_as_root = lambda: False
        self.dir = tempfile.mkdtemp(prefix="ws-pull-test-")
        self.state = os.path.join(self.dir, "state")
        self.archive = os.path.join(self.state, "archive")
        self.pwfile = os.path.join(self.dir, "password")
        Fake.password, Fake.requests = "", []
        self.config(self.url)

    def tearDown(self):
        shutil.rmtree(self.dir)

    def config(self, url, **extra):
        self.conf = os.path.join(self.dir, "ws-pull.conf")
        lines = ["[ws-pull]", f"url = {url}", f"state_dir = {self.state}", f"password_file = {self.pwfile}",
                 "retry_wait_s = 0"] + [f"{k} = {v}" for k, v in extra.items()]
        with open(self.conf, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")

    def pull(self):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = ws_pull.main(["--config", self.conf])
        return code, out.getvalue(), err.getvalue()

    def month(self, name):
        with open(os.path.join(self.archive, f"{name}.csv"), encoding="utf-8", newline="") as f:
            return list(csv.DictReader(f))

    def header(self, name):
        with open(os.path.join(self.archive, f"{name}.csv"), encoding="utf-8", newline="") as f:
            return next(csv.reader(f))

    def cursor(self):
        with open(os.path.join(self.state, "cursor"), encoding="utf-8") as f:
            return f.read().strip()

    def status(self):
        with open(os.path.join(self.archive, "status.json"), encoding="utf-8") as f:
            return json.load(f)

    # --- the normal path ---

    def test_first_pull_splits_months(self):
        answer([slot(OCT_LAST), slot(NOV_FIRST), slot(NOV_LATER, bme_p=None, bme_t=None, bme_rh=None)])
        code, out, err = self.pull()
        self.assertEqual(code, 0, err)
        self.assertIn("3 slots: 3 new", out)
        self.assertNotIn("cursor=", Fake.requests[0][0])
        self.assertIn("fmt=json", Fake.requests[0][0])
        oct_rows, nov_rows = self.month("2026-10"), self.month("2026-11")
        self.assertEqual([r["ts"] for r in oct_rows], [str(OCT_LAST)])
        self.assertEqual([r["ts"] for r in nov_rows], [str(NOV_FIRST), str(NOV_LATER)])
        r = oct_rows[0]
        self.assertEqual(r["time"], "2026-10-31T23:45:00Z")
        self.assertEqual((r["sht_t"], r["bme_p"], r["p_sea"], r["bat_mv"], r["late"]),
                         ("5.23", "1013.4", "1", "3912", "0"))
        self.assertEqual((nov_rows[1]["bme_p"], nov_rows[1]["p_sea"]), ("", ""))
        self.assertEqual(self.header("2026-11"), ws_pull.BASE_COLUMNS)
        self.assertEqual(self.cursor(), "0badcafe-10")
        st = self.status()
        self.assertTrue(st["ok"])
        self.assertEqual(st["newest_slot"], "2026-11-01T00:15:00Z")

    def test_cursor_sent_and_row_replaced(self):
        answer([slot(NOV_FIRST), slot(NOV_LATER)])
        self.assertEqual(self.pull()[0], 0)
        answer([slot(NOV_FIRST, sht_t=6.0)], cursor="0badcafe-12")
        code, out, err = self.pull()
        self.assertEqual(code, 0, err)
        self.assertIn("cursor=0badcafe-10", Fake.requests[1][0])
        self.assertIn("1 changed", out)
        rows = self.month("2026-11")
        self.assertEqual([r["sht_t"] for r in rows], ["6.0", "5.23"])
        self.assertEqual(self.cursor(), "0badcafe-12")

    def test_same_values_counted_as_same(self):
        answer([slot(NOV_FIRST)])
        self.pull()
        answer([slot(NOV_FIRST)], cursor="0badcafe-11")
        self.assertIn("1 the same", self.pull()[1])

    def test_new_column_grows_header(self):
        answer([slot(NOV_FIRST)])
        self.pull()
        answer([slot(NOV_LATER, s1_t=19.5, s1_rh=50.25)], cursor="0badcafe-11")
        code, _, err = self.pull()
        self.assertEqual(code, 0, err)
        self.assertEqual(self.header("2026-11"), ws_pull.BASE_COLUMNS + ["s1_t", "s1_rh"])
        rows = self.month("2026-11")
        self.assertEqual([(r["s1_t"], r["s1_rh"]) for r in rows], [("", ""), ("19.5", "50.25")])
        # and the extra column stays when a later answer lacks it
        answer([slot(NOV_FIRST, sht_t=7.0)], cursor="0badcafe-12")
        self.pull()
        self.assertEqual(self.header("2026-11"), ws_pull.BASE_COLUMNS + ["s1_t", "s1_rh"])

    def test_pressure_not_at_sea_level(self):
        answer([slot(NOV_FIRST, bme_p=995.1)], at_sea=False)
        self.pull()
        self.assertEqual(self.month("2026-11")[0]["p_sea"], "0")

    def test_password_sent(self):
        Fake.password = "pa:ss wörd"
        with open(self.pwfile, "w", encoding="utf-8") as f:
            f.write("pa:ss wörd\n")
        answer([slot(NOV_FIRST)])
        code, _, err = self.pull()
        self.assertEqual(code, 0, err)

    def test_display_restart_noted(self):
        answer([slot(NOV_FIRST)])
        self.pull()
        answer([slot(NOV_FIRST)], cursor="12345678-3")
        self.assertIn("restarted", self.pull()[1])

    def test_ts_out_of_range_skipped(self):
        answer([slot(NOV_FIRST), slot(1000), slot(4102444800)])  # 1970 and 2100
        code, out, err = self.pull()
        self.assertEqual(code, 0, err)
        self.assertIn("1 slot: 1 new", out)
        self.assertIn("2 slots skipped", err)

    def test_fields_made_here_ignored(self):
        answer([slot(NOV_FIRST, time="x", p_sea=1)])
        code, _, err = self.pull()
        self.assertEqual(code, 0, err)
        self.assertIn("'p_sea'", err)
        self.assertEqual(self.month("2026-11")[0]["time"], "2026-11-01T00:00:00Z")

    def test_nothing_new(self):
        answer([])
        code, out, _ = self.pull()
        self.assertEqual((code, out.strip()), (0, "nothing new"))

    # --- refusals: nothing written, the cursor stays ---

    def pull_then(self, prepare, text):
        """A good pull, then the answer prepare() sets up must be refused."""
        answer([slot(NOV_FIRST)])
        self.assertEqual(self.pull()[0], 0)
        before = self.month("2026-11")
        prepare()
        code, _, err = self.pull()
        self.assertEqual(code, 1)
        self.assertIn(text, err)
        self.assertEqual(self.month("2026-11"), before)
        self.assertEqual(self.cursor(), "0badcafe-10")
        self.assertFalse(self.status()["ok"])
        self.assertIsNotNone(self.status()["last_success"])

    def test_refuses_string_value(self):
        self.pull_then(lambda: answer([slot(NOV_LATER, sht_t="5.2; rm -rf /")], cursor="0badcafe-11"),
                       "not a number")

    def test_refuses_nan(self):
        def bad():
            Fake.body = (b'{"cursor":"0badcafe-11","pressure_at_sea_level":true,"slots":'
                         b'[{"ts":%d,"sht_t":NaN}]}' % NOV_LATER)
            Fake.xcursor = "0badcafe-11"
        self.pull_then(bad, "valid JSON")

    def test_refuses_bad_field_name(self):
        self.pull_then(lambda: answer([slot(NOV_LATER, **{"../../etc": 1})], cursor="0badcafe-11"),
                       "field name")

    def test_refuses_nested_value(self):
        self.pull_then(lambda: answer([slot(NOV_LATER, sht_t=[1, 2])], cursor="0badcafe-11"), "not a number")

    def test_refuses_bool_ts(self):
        self.pull_then(lambda: answer([slot(True)], cursor="0badcafe-11"), "whole-number ts")

    def test_refuses_cursor_mismatch(self):
        self.pull_then(lambda: answer([slot(NOV_LATER)], cursor="0badcafe-11", xcursor="0badcafe-99"),
                       "differ")

    def test_refuses_bad_cursor(self):
        self.pull_then(lambda: answer([slot(NOV_LATER)], cursor="../x"), "no valid cursor")

    def test_refuses_too_many_fields(self):
        extra = {f"x{i}": i for i in range(60)}
        self.pull_then(lambda: answer([slot(NOV_LATER, **extra)], cursor="0badcafe-11"), "more than 64")

    def test_refuses_deep_nesting(self):
        def bad():
            Fake.body = b"[" * 100000 + b"]" * 100000
            Fake.xcursor = ""
        self.pull_then(bad, "valid JSON")

    def test_refuses_too_big(self):
        self.config(self.url, max_bytes=2048)
        self.pull_then(lambda: answer([slot(NOV_FIRST + 900 * i) for i in range(1, 40)], cursor="0badcafe-11"),
                       "bigger than 2048")

    def test_refuses_wrong_content_type(self):
        def bad():
            answer([slot(NOV_LATER)], cursor="0badcafe-11")
            Fake.ctype = "text/html"
        self.pull_then(bad, "isn't JSON")

    def test_refuses_http_error(self):
        def bad():
            answer([slot(NOV_LATER)], cursor="0badcafe-11")
            Fake.status = 500
        self.pull_then(bad, "HTTP 500")

    def test_wrong_password(self):
        Fake.password = "secret"
        answer([slot(NOV_FIRST)])
        code, _, err = self.pull()
        self.assertEqual(code, 1)
        self.assertIn("401", err)
        self.assertFalse(os.path.exists(os.path.join(self.state, "cursor")))

    def test_unreachable_retries(self):
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()
        self.config(f"http://127.0.0.1:{port}", attempts=2, timeout_s=5)
        code, out, err = self.pull()
        self.assertEqual(code, 1)
        self.assertIn("try 1 of 2", out)
        self.assertIn("no answer", err)
        self.assertFalse(self.status()["ok"])

    # --- the archive ---

    def test_damaged_month_moved_aside(self):
        os.makedirs(self.archive)
        with open(os.path.join(self.archive, "2026-11.csv"), "w", encoding="utf-8") as f:
            f.write("not;our;file\n1;2;3\n")
        answer([slot(NOV_FIRST)])
        code, _, err = self.pull()
        self.assertEqual(code, 0, err)
        self.assertIn("moved to 2026-11.csv.damaged-", err)
        self.assertTrue(any(n.startswith("2026-11.csv.damaged-") for n in os.listdir(self.archive)))
        self.assertEqual(len(self.month("2026-11")), 1)
        self.assertEqual(self.cursor(), "")  # so the next run asks for everything

    def test_bad_config(self):
        self.config("ftp://x")
        code, _, err = self.pull()
        self.assertEqual(code, 2)
        self.assertIn("url must look like", err)

    def test_refuses_root(self):
        ws_pull.running_as_root = lambda: True
        self.assertEqual(self.pull()[0], 2)

    def test_cell_formats(self):
        self.assertEqual(ws_pull.cell("x", 1e-7), "0.0000001")
        self.assertEqual(ws_pull.cell("x", -0.5), "-0.5")
        self.assertEqual(ws_pull.cell("x", True), "1")
        self.assertEqual(ws_pull.cell("x", None), "")
        with self.assertRaises(ws_pull.Refused):
            ws_pull.cell("x", 1e300)


if __name__ == "__main__":
    unittest.main(verbosity=1)
