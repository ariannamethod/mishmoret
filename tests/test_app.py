"""Exercise the real C server through HTTP against disposable SQLite databases.

Run after make: python3 -m unittest discover -s tests -v
MISHMERET_BIN may select a sanitizer build of the same executable.
"""

import concurrent.futures
import datetime as dt
import http.client
import http.cookies
import json
import os
from pathlib import Path
import shutil
import socket
import sqlite3
import subprocess
import tempfile
import threading
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get("MISHMERET_BIN", ROOT / "build" / "mishmeret")).resolve()
SUNDAY = dt.date(2099, 1, 1)
SUNDAY += dt.timedelta(days=(6 - SUNDAY.weekday()) % 7)
WEEK = SUNDAY.isoformat()
DAYS = [(SUNDAY + dt.timedelta(days=i)).isoformat() for i in range(7)]


class Client:
    def __init__(self, port):
        self.port = port
        self.origin = f"http://127.0.0.1:{port}"
        self.cookies = {}
        self.csrf = None

    def request(self, method, path, data=None, *, origin=True, csrf=True, raw=None, chunks=None, forwarded=None):
        headers = {}
        if forwarded is not None:
            headers["X-Forwarded-For"] = forwarded
        if origin is not False:
            headers["Origin"] = self.origin if origin is True else origin
        if self.cookies:
            headers["Cookie"] = "; ".join(f"{k}={v}" for k, v in self.cookies.items())
        if csrf is not False and self.csrf:
            headers["X-CSRF-Token"] = self.csrf if csrf is True else csrf
        body = chunks if chunks is not None else raw
        if data is not None:
            body = json.dumps(data, ensure_ascii=False).encode("utf-8")
        if body is not None:
            headers["Content-Type"] = "application/json"
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        try:
            connection.request(method, path, body=body, headers=headers, encode_chunked=chunks is not None)
            response = connection.getresponse()
            content = response.read()
            pairs = response.getheaders()
            for name, value in pairs:
                if name.lower() == "set-cookie":
                    parsed = http.cookies.SimpleCookie()
                    parsed.load(value)
                    for key, morsel in parsed.items():
                        if morsel.value and morsel["max-age"] != "0":
                            self.cookies[key] = morsel.value
                        else:
                            self.cookies.pop(key, None)
            response_headers = {k.lower(): v for k, v in pairs}
            if "application/json" in response_headers.get("content-type", ""):
                payload = json.loads(content)
            else:
                payload = content.decode("utf-8", errors="replace")
            if isinstance(payload, dict) and payload.get("csrf"):
                self.csrf = payload["csrf"]
            return response.status, payload, response_headers
        finally:
            connection.close()


class AppTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not BINARY.is_file():
            raise RuntimeError(f"Build the C application first: missing {BINARY}")
        cls.seed_dir = tempfile.TemporaryDirectory(prefix="mishmeret-seed-")
        cls.seed_db = Path(cls.seed_dir.name) / "seed.db"
        result = subprocess.run(
            [str(BINARY), "--init", "--db", str(cls.seed_db)],
            capture_output=True, text=True, timeout=30,
        )
        if result.returncode:
            cls.seed_dir.cleanup()
            raise RuntimeError(f"Initialization failed: {result.stderr}")
        cls.accounts = {a["login"]: a for a in json.loads(result.stdout)["accounts"]}
        if set(cls.accounts) != {"oleg1", "oleg2", "shira", "reut"}:
            raise AssertionError("Initialization must create the four agreed administrators")
        passwords = [a["password"] for a in cls.accounts.values()]
        if len(set(passwords)) != 4 or any(len(p) < 12 for p in passwords):
            raise AssertionError("Initial administrator passwords must be unique and >=12 characters")

    @classmethod
    def tearDownClass(cls):
        cls.seed_dir.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mishmeret-test-")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.db = self.directory / "app.db"
        shutil.copy2(self.seed_db, self.db)
        self.passwords = {}
        self.process = None
        self.log = open(self.directory / "server.log", "w+b")
        self.addCleanup(self.log.close)
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            self.port = sock.getsockname()[1]
        self.addCleanup(self.stop_server)
        self.start_server()

    def start_server(self):
        self.process = subprocess.Popen(
            [str(BINARY), "--db", str(self.db), "--web", str(ROOT / "web"),
             "--port", str(self.port), "--origin", f"http://127.0.0.1:{self.port}"] + getattr(self, "server_options", []),
            stdout=self.log, stderr=self.log,
        )
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                self.fail(f"Server exited during startup ({self.process.returncode}): {self.logs()}")
            try:
                if Client(self.port).request("GET", "/healthz", origin=False)[0] == 200:
                    return
            except (OSError, http.client.HTTPException):
                time.sleep(0.03)
        self.fail(f"Server did not become healthy: {self.logs()}")

    def stop_server(self):
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
            self.assertEqual(self.process.returncode, 0, f"Unclean server shutdown: {self.logs()}")
            log = self.logs()
            for marker in ["ERROR: AddressSanitizer", "LeakSanitizer", "runtime error:"]:
                self.assertNotIn(marker, log)

    def logs(self):
        self.log.flush()
        self.log.seek(0)
        return self.log.read().decode("utf-8", errors="replace")

    def tearDown(self):
        self.assertIsNone(self.process.poll(), f"Server crashed: {self.logs()}")

    def ok(self, response, expected=200):
        status, data, _ = response
        self.assertEqual(status, expected, repr(data))
        return data

    def rejected(self, response, expected=None):
        status, data, _ = response
        if expected is not None:
            self.assertEqual(status, expected, repr(data))
        else:
            self.assertGreaterEqual(status, 400, repr(data))
            self.assertLess(status, 500, repr(data))
        return data

    def login(self, login="oleg2", password=None, change=True):
        client = Client(self.port)
        if password is None:
            password = self.passwords.get(login, self.accounts[login]["password"])
        session = self.ok(client.request("POST", "/api/login", {"login": login, "password": password}))
        if change and session["user"]["must_change_password"]:
            new_password = "Test-password-" + login + "-2026!"
            self.ok(client.request("POST", "/api/password", {
                "old_password": password, "new_password": new_password,
            }))
            self.passwords[login] = new_password
            session = self.ok(client.request("GET", "/api/session"))
            self.assertIsNotNone(session["user"], "Password change must keep the current session")
            self.assertFalse(session["user"]["must_change_password"])
        self.assertTrue(client.csrf)
        return client

    def users(self, admin):
        return self.ok(admin.request("GET", "/api/admin/users"))["users"]

    def member(self, admin, login="member", name="משתתף בדיקה"):
        created = self.ok(admin.request("POST", "/api/admin/users", {
            "login": login, "name": name, "role": "member",
        }), 201)
        return self.login(login, created["temporary_password"]), created["user"]

    def week(self, client):
        return self.ok(client.request("GET", f"/api/week?start={WEEK}"))

    def resource(self, admin, name="שולחן בדיקה", room="חדר בדיקה", kind="desk"):
        self.ok(admin.request("POST", "/api/admin/resources", {"name": name, "room": room, "kind": kind}))
        return next(r for r in self.week(admin)["resources"]
                    if r["name"] == name and r["room"] == room and r["kind"] == kind)

    def book(self, client, day=0, period="morning", location="center", resource_id=None, **extra):
        return client.request("POST", "/api/bookings", {
            "date": DAYS[day], "period": period, "location": location,
            "resource_id": resource_id, **extra,
        })

    def booking(self, client, user_id=None, day=0):
        rows = [r for r in self.week(client)["bookings"] if r["date"] == DAYS[day]
                and (user_id is None or r["user_id"] == user_id)]
        self.assertEqual(len(rows), 1, repr(rows))
        return rows[0]

    def test_initialization_refuses_to_replace_existing_accounts(self):
        admin = self.login()
        before = self.users(admin)
        self.stop_server()
        result = subprocess.run([str(BINARY), "--init", "--db", str(self.db)],
                                capture_output=True, text=True, timeout=20)
        self.start_server()
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('"accounts"', result.stdout)
        self.assertEqual(self.users(admin), before)
        self.login()

    def test_anonymous_requests_cannot_read_or_modify_private_data(self):
        client = Client(self.port)
        self.assertIsNone(self.ok(client.request("GET", "/api/session"))["user"])
        for method, path, data in [
            ("GET", f"/api/week?start={WEEK}", None),
            ("GET", "/api/admin/users", None),
            ("POST", "/api/bookings", {"date": DAYS[0], "period": "full", "location": "home"}),
            ("DELETE", "/api/bookings?id=1", None),
        ]:
            with self.subTest(method=method, path=path):
                self.rejected(client.request(method, path, data), 401)

    def test_temporary_password_requires_change_and_cookie_is_httponly(self):
        client = Client(self.port)
        response = client.request("POST", "/api/login", {
            "login": "oleg2", "password": self.accounts["oleg2"]["password"],
        })
        session = self.ok(response)
        self.assertTrue(session["user"]["must_change_password"])
        cookie = response[2].get("set-cookie", "").lower()
        self.assertIn("httponly", cookie)
        self.assertIn("samesite=strict", cookie)
        self.assertIn("path=/", cookie)
        self.rejected(client.request("GET", f"/api/week?start={WEEK}"), 403)
        self.rejected(client.request("GET", "/api/admin/users"), 403)
        self.rejected(self.book(client, location="home"), 403)
        for old, new in [("wrong-password", "Strong-password-123"),
                         (self.accounts["oleg2"]["password"], "short")]:
            self.rejected(client.request("POST", "/api/password", {"old_password": old, "new_password": new}))
        self.assertTrue(self.ok(client.request("GET", "/api/session"))["user"]["must_change_password"])

    def test_password_minimum_counts_characters_not_utf8_bytes(self):
        client = self.login(change=False)
        old = self.accounts["oleg2"]["password"]
        for short in ["a" * 11, "אבגדהו", "🙂" * 3]:
            with self.subTest(password=short):
                self.rejected(client.request("POST", "/api/password", {
                    "old_password": old, "new_password": short,
                }), 400)
        self.ok(client.request("POST", "/api/password", {
            "old_password": old, "new_password": "אבגדהוזחטיכל",
        }))
        self.login(password="אבגדהוזחטיכל")

    def test_login_requires_exact_origin(self):
        for origin in [False, "null", "https://attacker.invalid", f"http://127.0.0.1:{self.port}.attacker.invalid"]:
            with self.subTest(origin=origin):
                client = Client(self.port)
                self.rejected(client.request("POST", "/api/login", {
                    "login": "oleg2", "password": self.accounts["oleg2"]["password"],
                }, origin=origin), 403)
                self.assertIsNone(self.ok(client.request("GET", "/api/session"))["user"])

    def test_mutations_require_csrf_and_origin_even_with_valid_cookie(self):
        client = self.login()
        other = self.login("oleg1")
        self.assertNotEqual(client.csrf, other.csrf)
        payload = {"date": DAYS[0], "period": "morning", "location": "home", "resource_id": None}
        for options in [{"csrf": False}, {"csrf": "invalid-token"}, {"csrf": other.csrf}, {"origin": False},
                        {"origin": "https://attacker.invalid"}]:
            with self.subTest(options=options):
                self.rejected(client.request("POST", "/api/bookings", payload, **options), 403)
                self.assertEqual(self.week(client)["bookings"], [])
        self.ok(client.request("POST", "/api/bookings", payload))

    def test_sql_injection_does_not_authenticate_or_change_users(self):
        for login in ["' OR 1=1 --", "oleg2' --", "'; DROP TABLE users; --"]:
            with self.subTest(login=login):
                client = Client(self.port)
                self.rejected(client.request("POST", "/api/login", {
                    "login": login, "password": "anything-at-all",
                }))
                self.assertIsNone(self.ok(client.request("GET", "/api/session"))["user"])
        self.assertEqual(len(self.users(self.login())), 4)

    def test_member_cannot_use_any_administrator_endpoint(self):
        admin = self.login()
        member, user = self.member(admin)
        operations = [
            ("GET", "/api/admin/users", None),
            ("POST", "/api/admin/users", {"login": "intruder", "name": "Injected", "role": "admin"}),
            ("PATCH", "/api/admin/users", {"id": user["id"], "name": "Changed", "role": "admin", "active": True}),
            ("POST", "/api/admin/reset-password", {"id": 1}),
            ("POST", "/api/admin/closures", {"date": DAYS[0], "period": "full", "location": "all", "reason": "x"}),
            ("DELETE", "/api/admin/closures?id=1", None),
            ("POST", "/api/admin/announcements", {"date": DAYS[0], "end_date": DAYS[1], "title": "x", "body": "x"}),
            ("DELETE", "/api/admin/announcements?id=1", None),
            ("POST", "/api/admin/resources", {"name": "x", "room": "x", "kind": "desk"}),
            ("PATCH", "/api/admin/resources", {"id": 1, "active": False}),
        ]
        for method, path, body in operations:
            with self.subTest(method=method, path=path):
                self.rejected(member.request(method, path, body), 403)
        persisted = next(u for u in self.users(admin) if u["id"] == user["id"])
        self.assertEqual(persisted["role"], "member")
        self.assertEqual(len(self.users(admin)), 5)

    def test_forged_user_id_never_changes_another_persons_booking(self):
        admin = self.login()
        admin_id = self.ok(admin.request("GET", "/api/session"))["user"]["id"]
        self.ok(self.book(admin, period="afternoon", location="home"))
        before = self.booking(admin, admin_id)
        member, user = self.member(admin)
        result = self.book(member, period="morning", location="home", user_id=admin_id)
        if result[0] != 200:
            self.rejected(result, 400)
        else:
            self.assertEqual(self.booking(admin, user["id"])["period"], "morning")
        self.assertEqual(self.booking(admin, admin_id), before)

    def test_booking_delete_checks_owner_but_allows_admin(self):
        admin = self.login()
        first, first_user = self.member(admin, "member1")
        second, _ = self.member(admin, "member2")
        self.ok(self.book(first, location="home"))
        record = self.booking(admin, first_user["id"])
        self.rejected(second.request("DELETE", f'/api/bookings?id={record["id"]}'), 403)
        self.assertEqual(self.booking(admin, first_user["id"]), record)
        self.ok(admin.request("DELETE", f'/api/bookings?id={record["id"]}'))
        self.assertEqual(self.week(admin)["bookings"], [])
        self.ok(self.book(first, location="home"))
        record = self.booking(admin, first_user["id"])
        self.ok(first.request("DELETE", f'/api/bookings?id={record["id"]}'))
        self.assertEqual(self.week(admin)["bookings"], [])

    def test_failed_replacement_keeps_original_booking_atomic(self):
        first, second = self.login(), self.login("oleg1")
        resource = self.resource(first)
        self.ok(self.book(first, resource_id=resource["id"]))
        self.ok(self.book(second, period="afternoon", location="home"))
        second_id = self.ok(second.request("GET", "/api/session"))["user"]["id"]
        before = self.booking(first, second_id)
        self.rejected(self.book(second, resource_id=resource["id"]), 409)
        self.assertEqual(self.booking(first, second_id), before)
        self.rejected(self.book(second, location="home", resource_id=resource["id"]), 400)
        self.assertEqual(self.booking(first, second_id), before)
        self.rejected(self.book(second, resource_id=999999), 400)
        self.assertEqual(self.booking(first, second_id), before)
        self.ok(self.book(second, period="full", location="home"))
        self.assertEqual(self.booking(first, second_id)["period"], "full")
        self.assertEqual(len(self.week(first)["bookings"]), 2)

    def test_full_day_overlaps_both_halves_but_separate_halves_coexist(self):
        first, second, third = self.login(), self.login("oleg1"), self.login("shira")
        desk = self.resource(first)
        self.ok(self.book(first, resource_id=desk["id"]))
        self.ok(self.book(second, period="afternoon", resource_id=desk["id"]))
        self.rejected(self.book(third, period="full", resource_id=desk["id"]), 409)
        self.assertEqual(len(self.week(first)["bookings"]), 2)
        self.ok(self.book(first, day=1, period="full", resource_id=desk["id"]))
        for half in ["morning", "afternoon"]:
            self.rejected(self.book(second, day=1, period=half, resource_id=desk["id"]), 409)

    def test_serialized_http_resource_requests_have_exactly_one_winner(self):
        first, second = self.login(), self.login("oleg1")
        desk = self.resource(first)
        barrier = threading.Barrier(2)

        def attempt(client):
            barrier.wait(timeout=5)
            return self.book(client, period="full", resource_id=desk["id"])

        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            outcomes = list(pool.map(attempt, [first, second]))
        self.assertEqual(sorted(r[0] for r in outcomes), [200, 409], repr(outcomes))
        rows = self.week(first)["bookings"]
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["resource_id"], desk["id"])

    def test_whole_room_conflicts_with_desks_but_distinct_desks_coexist(self):
        first, second, third = self.login(), self.login("oleg1"), self.login("shira")
        desk1 = self.resource(first, name="שולחן א")
        desk2 = self.resource(first, name="שולחן ב")
        room = self.resource(first, name="כל החדר", kind="room")
        self.ok(self.book(first, resource_id=desk1["id"]))
        self.ok(self.book(second, resource_id=desk2["id"]))
        self.rejected(self.book(third, resource_id=room["id"]), 409)
        for booking in self.week(first)["bookings"]:
            self.ok(first.request("DELETE", f'/api/bookings?id={booking["id"]}'))
        self.ok(self.book(third, period="full", resource_id=room["id"]))
        self.rejected(self.book(first, resource_id=desk1["id"]), 409)
        self.rejected(self.book(second, period="afternoon", resource_id=desk2["id"]), 409)

    def test_closure_marks_existing_booking_and_reopening_restores_it(self):
        admin, other = self.login(), self.login("oleg1")
        self.ok(self.book(admin, period="full", location="home"))
        before = self.booking(admin)
        self.ok(admin.request("POST", "/api/admin/closures", {
            "date": DAYS[0], "period": "full", "location": "all", "reason": "המרכז סגור",
        }))
        blocked = self.booking(admin)
        self.assertEqual(blocked["id"], before["id"])
        self.assertTrue(blocked["blocked"])
        self.rejected(self.book(other, location="home"), 409)
        self.rejected(self.book(admin, period="morning", location="home"), 409)
        closure = self.week(admin)["closures"][0]
        self.ok(admin.request("DELETE", f'/api/admin/closures?id={closure["id"]}'))
        restored = self.booking(admin)
        self.assertEqual(restored, before)
        self.assertFalse(restored["blocked"])

    def test_closure_matches_location_and_overlapping_period_only(self):
        admin, second, third = self.login(), self.login("oleg1"), self.login("shira")
        self.ok(self.book(admin, period="full", location="center"))
        self.ok(self.book(second, period="full", location="home"))
        self.ok(self.book(third, period="morning", location="home"))
        self.ok(admin.request("POST", "/api/admin/closures", {
            "date": DAYS[0], "period": "afternoon", "location": "home", "reason": "בדיקה",
        }))
        rows = self.week(admin)["bookings"]
        self.assertEqual(len(rows), 3)
        for row in rows:
            self.assertEqual(bool(row["blocked"]), row["location"] == "home" and row["period"] == "full")

    def test_stored_markup_and_sql_text_round_trip_as_literal_strings(self):
        admin = self.login()
        title = '<img src=x onerror="alert(1)">'
        body = "שלום '); DROP TABLE bookings; -- <script>alert(1)</script>"
        self.ok(admin.request("POST", "/api/admin/announcements", {
            "date": DAYS[0], "end_date": DAYS[1], "title": title, "body": body,
        }))
        announcement = self.week(admin)["announcements"][0]
        self.assertEqual(announcement["title"], title)
        self.assertEqual(announcement["body"], body)
        self.ok(self.book(admin, location="home"))
        self.assertEqual(len(self.week(admin)["bookings"]), 1)
        self.ok(admin.request("DELETE", f'/api/admin/announcements?id={announcement["id"]}'))
        self.assertEqual(self.week(admin)["announcements"], [])

    def test_announcement_accepts_exact_unicode_codepoint_limits(self):
        admin = self.login()
        for label, pattern in [("ASCII", "a"), ("Hebrew", "א"), ("emoji", "🙂"), ("mixed", "Aא🙂")]:
            with self.subTest(text=label):
                title = (pattern * 120)[:120]
                body = (pattern * 1000)[:1000]
                self.ok(admin.request("POST", "/api/admin/announcements", {
                    "date": DAYS[0], "end_date": DAYS[0], "title": title, "body": body,
                }))
                stored = next(row for row in self.week(admin)["announcements"] if row["title"] == title)
                self.assertEqual(stored["title"], title)
                self.assertEqual(stored["body"], body)

    def test_announcement_rejects_one_extra_codepoint_without_changing_data(self):
        admin = self.login()
        self.ok(admin.request("POST", "/api/admin/announcements", {
            "date": DAYS[0], "end_date": DAYS[0], "title": "קיים", "body": "ללא שינוי",
        }))
        for label, pattern in [("ASCII", "a"), ("Hebrew", "א"), ("emoji", "🙂"), ("mixed", "Aא🙂")]:
            for field, limit in [("title", 120), ("body", 1000)]:
                with self.subTest(text=label, field=field):
                    payload = {"date": DAYS[0], "end_date": DAYS[0], "title": "כותרת", "body": "תוכן"}
                    payload[field] = (pattern * (limit + 1))[:limit + 1]
                    before = self.week(admin)["announcements"]
                    response = admin.request("POST", "/api/admin/announcements", payload)
                    after = self.week(admin)["announcements"]
                    self.assertEqual((response[0], response[1].get("error"), after),
                                     (400, "invalid_announcement", before))

    def test_resource_with_future_booking_cannot_be_disabled(self):
        admin = self.login()
        desk = self.resource(admin)
        self.ok(self.book(admin, resource_id=desk["id"]))
        self.rejected(admin.request("PATCH", "/api/admin/resources", {"id": desk["id"], "active": False}), 409)
        resource = next(r for r in self.week(admin)["resources"] if r["id"] == desk["id"])
        self.assertTrue(resource["active"])
        booking = self.booking(admin)
        self.ok(admin.request("DELETE", f'/api/bookings?id={booking["id"]}'))
        self.ok(admin.request("PATCH", "/api/admin/resources", {"id": desk["id"], "active": False}))
        self.rejected(self.book(admin, resource_id=desk["id"]))
        self.assertEqual(self.week(admin)["bookings"], [])

    def test_deactivating_user_revokes_sessions_and_prevents_login(self):
        admin = self.login()
        member, user = self.member(admin)
        self.ok(admin.request("PATCH", "/api/admin/users", {
            "id": user["id"], "name": user["name"], "role": "member", "active": False,
        }))
        self.assertIsNone(self.ok(member.request("GET", "/api/session"))["user"])
        self.rejected(self.book(member, location="home"), 401)
        self.rejected(Client(self.port).request("POST", "/api/login", {
            "login": user["login"], "password": self.passwords[user["login"]],
        }), 401)

    def test_admin_password_reset_revokes_old_sessions_and_requires_change(self):
        admin = self.login()
        member, user = self.member(admin)
        reset = self.ok(admin.request("POST", "/api/admin/reset-password", {"id": user["id"]}))
        self.assertGreaterEqual(len(reset["temporary_password"]), 12)
        self.assertIsNone(self.ok(member.request("GET", "/api/session"))["user"])
        self.rejected(Client(self.port).request("POST", "/api/login", {
            "login": user["login"], "password": self.passwords[user["login"]],
        }), 401)
        replacement = self.login(user["login"], reset["temporary_password"], change=False)
        self.assertTrue(self.ok(replacement.request("GET", "/api/session"))["user"]["must_change_password"])
        self.rejected(self.book(replacement, location="home"), 403)

    def test_password_change_revokes_other_sessions_and_logout_revokes_current(self):
        first = self.login()
        second = self.login()
        self.ok(first.request("POST", "/api/password", {
            "old_password": self.passwords["oleg2"], "new_password": "Another-strong-password-2026!",
        }))
        self.assertIsNotNone(self.ok(first.request("GET", "/api/session"))["user"])
        self.assertIsNone(self.ok(second.request("GET", "/api/session"))["user"])
        stolen_cookies = dict(first.cookies)
        self.ok(first.request("POST", "/api/logout", {}))
        self.assertIsNone(self.ok(first.request("GET", "/api/session"))["user"])
        replay = Client(self.port)
        replay.cookies = stolen_cookies
        self.assertIsNone(self.ok(replay.request("GET", "/api/session"))["user"])

    def test_last_active_administrator_cannot_be_demoted_or_disabled(self):
        admin = self.login()
        current_id = self.ok(admin.request("GET", "/api/session"))["user"]["id"]
        for user in self.users(admin):
            if user["id"] != current_id:
                self.ok(admin.request("PATCH", "/api/admin/users", {
                    "id": user["id"], "name": user["name"], "role": "member", "active": True,
                }))
        current = next(u for u in self.users(admin) if u["id"] == current_id)
        for role, active in [("member", True), ("admin", False)]:
            self.rejected(admin.request("PATCH", "/api/admin/users", {
                "id": current_id, "name": current["name"], "role": role, "active": active,
            }), 409)
        remaining = [u for u in self.users(admin) if u["role"] == "admin" and u["active"]]
        self.assertEqual([u["id"] for u in remaining], [current_id])

    def test_demoted_administrators_lose_privileges_in_existing_sessions(self):
        admin, other = self.login(), self.login("oleg1")
        other_user = self.ok(other.request("GET", "/api/session"))["user"]
        self.ok(admin.request("PATCH", "/api/admin/users", {
            "id": other_user["id"], "name": other_user["name"], "role": "member", "active": True,
        }))
        response = other.request("GET", "/api/admin/users")
        self.assertIn(response[0], [401, 403], repr(response))
        other = self.login("oleg1")
        self.rejected(other.request("GET", "/api/admin/users"), 403)
        self.ok(self.book(other, location="home"))

    def test_tampering_with_session_cookie_never_authenticates(self):
        client = self.login()
        self.assertTrue(client.cookies)
        for key in list(client.cookies):
            client.cookies[key] = "0" * 64
        self.assertIsNone(self.ok(client.request("GET", "/api/session"))["user"])
        self.rejected(self.book(client, location="home"), 401)

    def test_invalid_dates_and_enums_do_not_replace_existing_booking(self):
        admin = self.login()
        self.ok(self.book(admin, location="home"))
        before = self.week(admin)["bookings"]
        invalid = [
            {"date": "2099-02-30"}, {"date": "2099-1-1"}, {"date": DAYS[5]},
            {"date": DAYS[6]}, {"date": "2020-01-01"}, {"date": None},
            {"period": "evening"}, {"location": "somewhere"}, {"resource_id": "1 OR 1=1"},
        ]
        for change in invalid:
            with self.subTest(change=change):
                self.rejected(admin.request("POST", "/api/bookings", {
                    "date": DAYS[0], "period": "morning", "location": "home", "resource_id": None, **change,
                }), 400)
                self.assertEqual(self.week(admin)["bookings"], before)
        self.rejected(admin.request("GET", f"/api/week?start={DAYS[1]}"), 400)

    def test_malformed_and_oversized_json_fail_without_crashing(self):
        admin = self.login()
        for raw in [b'{"date":', b'[]', b'null', b'{"date":"\\u0000"}']:
            with self.subTest(raw=raw):
                self.rejected(admin.request("POST", "/api/bookings", raw=raw), 400)
        self.rejected(admin.request("POST", "/api/bookings", raw=b'{"data":"' + b'x' * 17000 + b'"}'), 413)
        self.ok(self.book(admin, location="home"))

    def test_oversized_chunked_body_returns_413_without_changing_data(self):
        admin = self.login()
        self.ok(self.book(admin, period="full", location="home"))
        before = self.week(admin)["bookings"]
        chunks = iter([b'{"data":"', *([b"x" * 1024] * 20), b'"}'])
        self.rejected(admin.request("POST", "/api/bookings", chunks=chunks), 413)
        self.assertEqual(self.week(admin)["bookings"], before)
        self.ok(self.book(admin, period="morning", location="home"))

    def test_responses_disable_caching_and_assets_use_content_security_policy(self):
        client = self.login()
        for path in ["/api/session", f"/api/week?start={WEEK}", "/api/admin/users"]:
            with self.subTest(path=path):
                response = client.request("GET", path)
                self.ok(response)
                self.assertIn("no-store", response[2].get("cache-control", ""))
                self.assertEqual(response[2].get("x-content-type-options"), "nosniff")
        response = client.request("GET", "/")
        self.ok(response)
        csp = response[2].get("content-security-policy", "")
        self.assertIn("default-src", csp)
        self.assertNotIn("unsafe-inline", csp)
        self.assertNotIn("unsafe-eval", csp)
        self.assertIn("frame-ancestors", csp)

    def test_path_traversal_cannot_read_database_or_arbitrary_files(self):
        client = self.login()
        for path in ["/../app.db", "/%2e%2e/app.db", "/../etc/passwd", "/.git/config", "/app.db"]:
            with self.subTest(path=path):
                response = client.request("GET", path)
                self.rejected(response)
                self.assertNotIn("root:x:", str(response[1]))
                self.assertNotIn("SQLite format", str(response[1]))

    def test_bookings_and_announcements_survive_server_restart(self):
        admin = self.login()
        self.ok(self.book(admin, period="full", location="home"))
        self.ok(admin.request("POST", "/api/admin/announcements", {
            "date": DAYS[0], "end_date": DAYS[0], "title": "מחר פתוח", "body": "שעות רגילות",
        }))
        before = self.week(admin)
        self.stop_server()
        self.start_server()
        after = self.week(self.login())
        self.assertEqual(after["bookings"], before["bookings"])
        self.assertEqual(after["announcements"], before["announcements"])

    def test_online_backup_and_stopped_restore_preserve_snapshot_revoke_sessions(self):
        admin = self.login()
        self.ok(self.book(admin, period="morning", location="home"))
        self.ok(admin.request("POST", "/api/admin/announcements", {
            "date": DAYS[0], "end_date": DAYS[0], "title": "גיבוי", "body": "לפני העדכון",
        }))
        snapshot = self.week(admin)
        backup = self.directory / "backup.db"
        result = subprocess.run([str(BINARY), "--backup", str(backup), "--db", str(self.db)],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(backup.is_file())
        self.assertEqual(backup.stat().st_mode & 0o777, 0o600)
        self.ok(self.book(admin, period="afternoon", location="home"))
        self.ok(admin.request("POST", "/api/admin/announcements", {
            "date": DAYS[1], "end_date": DAYS[1], "title": "חדש", "body": "אחרי הגיבוי",
        }))
        changed = self.week(admin)
        live_restore = subprocess.run([str(BINARY), "--restore", str(backup), "--db", str(self.db)],
                                      capture_output=True, text=True, timeout=15)
        self.assertNotEqual(live_restore.returncode, 0)
        self.assertEqual(self.week(admin)["bookings"], changed["bookings"])
        self.stop_server()
        restore = subprocess.run([str(BINARY), "--restore", str(backup), "--db", str(self.db)],
                                 capture_output=True, text=True, timeout=15)
        self.start_server()
        self.assertEqual(restore.returncode, 0, restore.stderr)
        self.assertIsNone(self.ok(admin.request("GET", "/api/session"))["user"])
        restored = self.week(self.login())
        self.assertEqual(restored["bookings"], snapshot["bookings"])
        self.assertEqual(restored["announcements"], snapshot["announcements"])

    def test_invalid_backup_does_not_change_existing_database(self):
        admin = self.login()
        self.ok(self.book(admin, period="full", location="home"))
        snapshot = self.week(admin)
        corrupt = self.directory / "corrupt.db"
        corrupt.write_bytes(b"This is not a SQLite database.")
        unrelated = self.directory / "unrelated.db"
        with sqlite3.connect(unrelated) as database:
            database.executescript("CREATE TABLE unrelated(value TEXT); PRAGMA user_version=1;")
        self.stop_server()
        before = self.db.read_bytes()
        outcomes = []
        for source in [corrupt, unrelated]:
            result = subprocess.run([str(BINARY), "--restore", str(source), "--db", str(self.db)],
                                    capture_output=True, text=True, timeout=15)
            outcomes.append((source.name, result.returncode, self.db.read_bytes() == before))
        self.start_server()
        for name, status, preserved in outcomes:
            with self.subTest(source=name):
                self.assertNotEqual(status, 0)
                self.assertTrue(preserved, "Rejected backup changed the target database")
        self.assertEqual(self.week(admin)["bookings"], snapshot["bookings"])

    def test_second_server_cannot_open_the_same_database_on_another_port(self):
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            other_port = sock.getsockname()[1]
        result = subprocess.run([
            str(BINARY), "--db", str(self.db), "--web", str(ROOT / "web"),
            "--port", str(other_port), "--origin", f"http://127.0.0.1:{other_port}",
        ], capture_output=True, text=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.ok(Client(self.port).request("GET", "/healthz", origin=False))
        self.ok(self.book(self.login(), location="home"))

    def test_wrong_passwords_do_not_lock_the_owner_out(self):
        attacker = Client(self.port)
        for _ in range(8):
            self.rejected(attacker.request("POST", "/api/login", {
                "login": "oleg2", "password": "wrong-password-123",
            }), 401)
        # Even the same source still gets its valid password checked after eight errors.
        self.ok(attacker.request("POST", "/api/login", {
            "login": "oleg2", "password": self.accounts["oleg2"]["password"],
        }))
        self.stop_server()
        with sqlite3.connect(self.db) as database:
            database.execute("UPDATE users SET fail_count=99,fail_since=strftime('%s','now')")
        self.start_server()
        self.login()

    def test_untrusted_forwarded_headers_cannot_reset_hash_budget(self):
        attacker = Client(self.port)
        statuses = []
        for i in range(26):
            statuses.append(attacker.request("POST", "/api/login", {
                "login": "oleg2", "password": "wrong-password-123",
            }, forwarded=f"192.0.2.{i+1}")[0])
        self.assertIn(429, statuses)
        self.assertEqual(statuses[-1], 429)
        self.rejected(attacker.request("POST", "/api/login", {
            "login": "oleg2", "password": self.accounts["oleg2"]["password"],
        }, forwarded="192.0.2.254"), 429)

    def test_tailscale_proxy_limit_uses_validated_client_address(self):
        self.stop_server()
        self.server_options = ["--trust-tailscale-proxy"]
        self.start_server()
        client = Client(self.port)
        payload = {"login": "oleg2", "password": "wrong-password-123"}
        for _ in range(26):
            response = client.request("POST", "/api/login", payload, forwarded="192.0.2.1")
        self.rejected(response, 429)
        # A mapped IPv4 address must not open another bucket for the same client.
        self.rejected(client.request("POST", "/api/login", payload,
                                     forwarded="::ffff:192.0.2.1"), 429)
        self.ok(client.request("POST", "/api/login", {
            "login": "oleg2", "password": self.accounts["oleg2"]["password"],
        }, forwarded="192.0.2.2"))
        for forged in ["192.0.2.1, 192.0.2.2", "garbage", "192.0.2.1:1234"]:
            self.rejected(client.request("POST", "/api/login", payload, forwarded=forged), 429)
        for i in range(26):
            response = client.request("POST", "/api/login", payload,
                                      forwarded=f"2001:db8:1:2::{i+1:x}")
        self.rejected(response, 429)  # IPv6 privacy addresses share the /64 bucket.
        self.rejected(client.request("POST", "/api/login", payload,
                                     forwarded="2001:db8:1:3::1"), 401)

    def test_password_change_rotates_session_and_csrf_and_revokes_other_sessions(self):
        client = self.login()
        other = self.login()
        stolen = Client(self.port)
        stolen.cookies = client.cookies.copy()
        stolen.csrf = client.csrf
        old_csrf = client.csrf
        self.ok(client.request("POST", "/api/password", {
            "old_password": self.passwords["oleg2"], "new_password": "Changed-password-2026!",
        }))
        self.assertNotEqual(client.cookies, stolen.cookies)
        self.assertNotEqual(client.csrf, old_csrf)
        self.assertIsNone(self.ok(stolen.request("GET", "/api/session"))["user"])
        self.assertIsNone(self.ok(other.request("GET", "/api/session"))["user"])
        self.rejected(client.request(
            "POST", "/api/bookings", {"date": DAYS[0], "period": "morning", "location": "home"},
            csrf=old_csrf), 403)
        self.ok(self.book(client, location="home"))

    def test_booking_failure_rolls_back_a_partially_applied_statement(self):
        admin = self.login()
        # RAISE(FAIL) preserves the statement's earlier changes unless the caller rolls back.
        with sqlite3.connect(self.db) as database:
            database.execute("CREATE TRIGGER injected_failure AFTER INSERT ON bookings BEGIN "
                             "SELECT RAISE(FAIL, 'injected write failure'); END")
        self.rejected(self.book(admin, location="home"), 500)
        self.assertEqual(self.week(admin)["bookings"], [])
        with sqlite3.connect(self.db) as database:
            database.execute("DROP TRIGGER injected_failure")
        self.ok(self.book(admin, location="home"))

    def test_restore_rejects_backup_trigger_without_touching_destination(self):
        admin = self.login()
        self.ok(self.book(admin, location="home"))
        snapshot = self.week(admin)
        crafted = self.directory / "trigger-backup.db"
        result = subprocess.run([str(BINARY), "--backup", str(crafted), "--db", str(self.db)],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        with sqlite3.connect(crafted) as database:
            database.executescript(
                "CREATE TRIGGER corrupt_on_session_delete AFTER DELETE ON sessions BEGIN "
                "INSERT INTO bookings(user_id,date,mask,location) "
                f"VALUES(999999,'{DAYS[1]}',1,'home'); END;"
            )
        self.stop_server()
        before = self.db.read_bytes()
        restore = subprocess.run([str(BINARY), "--restore", str(crafted), "--db", str(self.db)],
                                 capture_output=True, text=True, timeout=15)
        unchanged = self.db.read_bytes() == before
        self.start_server()
        self.assertNotEqual(restore.returncode, 0)
        self.assertTrue(unchanged, "Rejected trigger-bearing backup changed the live database")
        self.assertEqual(self.week(admin)["bookings"], snapshot["bookings"])

    def test_database_aliases_cannot_bypass_server_or_restore_lock(self):
        admin = self.login()
        self.ok(self.book(admin, location="home"))
        snapshot = self.week(admin)
        backup = self.directory / "alias-backup.db"
        result = subprocess.run([str(BINARY), "--backup", str(backup), "--db", str(self.db)],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        symlink = self.directory / "symlink.db"
        symlink.symlink_to(self.db)
        hardlink = self.directory / "hardlink.db"
        for alias in [symlink, hardlink]:
            with self.subTest(alias=alias.name):
                if alias == hardlink:
                    os.link(self.db, hardlink)
                try:
                    with socket.socket() as sock:
                        sock.bind(("127.0.0.1", 0))
                        other_port = sock.getsockname()[1]
                    server = subprocess.run([
                        str(BINARY), "--db", str(alias), "--web", str(ROOT / "web"),
                        "--port", str(other_port), "--origin", f"http://127.0.0.1:{other_port}",
                    ], capture_output=True, text=True, timeout=5)
                    self.assertNotEqual(server.returncode, 0)
                    restore = subprocess.run([
                        str(BINARY), "--restore", str(backup), "--db", str(alias),
                    ], capture_output=True, text=True, timeout=15)
                    self.assertNotEqual(restore.returncode, 0)
                finally:
                    if alias == hardlink:
                        hardlink.unlink()
                self.assertEqual(self.week(admin)["bookings"], snapshot["bookings"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
