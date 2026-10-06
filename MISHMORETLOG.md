# Mishmoret work log

Newest entries first. Preserve previous results and append dated corrections.

## 2026-10-06 — Astra: bounded follow-up to Claude's re-audit

- Removed unused account-failure reads/writes. Legacy columns remain untouched
  for database/backup compatibility. Source-based KDF limits are unchanged.
- Password-change session secrets are now generated after successful hashing
  and BEGIN, so the earlier 503/BEGIN-error exits have no new token to retain.
  The existing success and rollback paths erase the generated token.
- Extended the existing static/CSP test to fetch /itzik.svg anonymously and
  verify HTTP 200, exact SVG contents, image/svg+xml and the CSP header.
- Passed 61 integration tests; four relevant auth/asset tests also passed with
  ASan/UBSan and leak detection. Previous Chromium run already covers the final
  Reut-only nickname and neutral management labels; frontend is unchanged here.
- Completed a bounded UTF-8-preserving mutation run against the embedded Wolfe
  ABI in neural mode under ASan/UBSan on macOS ARM64. Fixed seed 20261006;
  2000 inputs, each valid UTF-8 and <=512 bytes, including the 239 corpus seeds.
  Results: 642 call, 371 no_call, 934 missing_arguments, 52 ambiguous; one input
  rejected by the existing 95-byte token limit. No sanitizer reports. Recorded
  input #797 and the rejection; this is a resource-limit result, not a crash.
  The output collector initially counted serialization's blank lines as records;
  final accounting uses the numbered nonempty records. An initial zero-error
  assertion also surfaced the documented token-limit rejection; raw output is
  preserved. This run checks bounded memory/runtime behavior, not the semantic
  correctness of arbitrary mutated requests or exhaustive parser coverage.
- Did not introduce an account-wide pre-password limiter or a shared overflow
  bucket. Those are design proposals: preserving valid-owner access would need
  a nonblocking approach, and a shared fallback remains a shared contention
  point. Full active source tables still reject new sources as documented.
- Deployed this bounded cleanup to Polygon after backup
  mishmeret-20261006T031403Z-1123926.db; public health returned 200.

## 2026-10-06 — Astra: Itzik becomes the project logo; publication authorized

- Oleg authorized committing/pushing the completed repairs and asked to make
  Itzik's face the project logo. Exported the existing vector portrait to
  web/itzik.svg, added it to README, the site branding and the browser favicon.
  The animated chat portrait remains available. The static HTTP allowlist now
  serves exactly /itzik.svg with image/svg+xml.
- Passed all 61 integration tests and Chromium desktop/mobile checks on the
  final source. Inspected the logo in the desktop browser screenshot. Deployed
  the build to Polygon and verified the public SVG matches the local file.
- Clarified current cancellation behavior: Itzik has no cancellation tool;
  the website/API allow an owner or administrator to delete a booking. Oleg
  will ask Reut whether cancellation must be restricted to her. No attendance
  policy was changed pending that answer.
- The previous Opus archive remains the security-audit snapshot. Subsequent
  logo changes touch README, static branding, the asset allowlist, API docs and
  this log; the security and inference implementations are unchanged.

## 2026-10-06 — Astra: audit repairs and colloquial Itzik on Polygon

- Confirmed the account lock and global KDF bucket in the code reviewed by Don.
  Replaced them with a bounded per-source bucket (20 tokens, 0.2 tokens/second,
  two tokens for password change). IPv6 uses /64; active buckets are not evicted.
  Existing account failure counts no longer block owners. Correct credentials
  on an inactive account no longer increment the wrong-password counter.
- Tested installed Tailscale 1.102.4 through a temporary isolated Funnel listener:
  normal and duplicate spoofed X-Forwarded-For headers reached the probe as the
  same single actual client address. Removed the temporary listener afterwards;
  the existing 443 route was preserved. The app trusts this header only from
  loopback and only with an explicit opt-in. Polygon now has that opt-in enabled.
- Password changes atomically rotate the current cookie and CSRF token and
  revoke other sessions. Removed the bootstrap login inventory from README.
- Corrected the earlier transaction-test claim: concurrent HTTP requests are
  serialized, so that test checks resource outcomes, not transaction necessity.
  Added a partial-statement failure test using SQLite RAISE(FAIL); removing the
  booking transaction makes this test fail with a persisted unwanted booking.
- Added 82 Hebrew corpus examples plus spoken/spelling aliases (including
  תקבע לי, חממה, צהרים, אחהצ and באנטר). The corpus now has 239 examples.
  The exact short request יום שלישי חממה צהרים proposes Tuesday afternoon in
  the center; held-out word orders and negative/conflicting requests are checked.
  A contradictory clarification no longer remains pending indefinitely. A
  location correction לא, מהבית is accepted only when location is pending.
- Browser tests now exercise actual bare-fragment completion, three-message
  completion, contradiction recovery and the colloquial Wednesday request.
  UI uses דיקטטרורית and אנטר, באר שבע; the prompt now uses תקבע לי.
- Passed 61 integration tests normally and under ASan/UBSan with leak detection;
  Chromium desktop/mobile passed. Two auth checks were then made portable to
  macOS by avoiding alternate loopback aliases, and rerun successfully. Fixed
  boundary parser inputs were included; no open-ended fuzz campaign was run.
- Deployed tested binary/source/corpus and the proxy setting to Polygon.
  Public relay health is 200; the served page has the new Hebrew labels.
  Backup: mishmeret-20261006T022445Z-1122180.db. Previous binary retained as
  build/mishmeret-before-oct06-audit-repair. No commit or push performed.
- The per-source limit addresses single-source KDF starvation; people sharing
  a NAT share a bucket. Counters reset on restart; this is not a distributed
  traffic defense. Attendance time cutoffs, past deletion and shared names were
  not changed: the original group brief explicitly asks who attends together,
  and defines no morning/afternoon clock cutoffs.

## 2026-10-06 — Astra: Itzik's first live Hebrew corrections

- Reproduced Oleg's live requests in the embedded Wolfe CLI: the Tuesday/morning
  request returned `missing:["location"]`; its separate clarification returned
  `no_call`; the Thursday/afternoon request with `ב מרכז` also lacked location.
- Added location aliases `מהמרכז`, `ב מרכז`, `ב חממה`, retaining earlier forms.
  A single missing field now receives its own Hebrew question.
- The browser retains one unfinished request. It parses each next message
  independently first; only `no_call` retries with that unfinished text. New
  recognized requests take precedence. Combined input stays within 512 bytes;
  session/week changes, manual form actions and errors clear pending context.
- Changed displayed location labels to `בחממה` and the input prompt to masculine
  `תרשום`. Existing feminine input examples remain accepted.
- Passed 54 integration tests and the Chromium flow including Oleg's exact
  clarification, a new independent Thursday request, negation, and week-context
  reset. Proposals still create no bookings before confirmation.
- Backed up the live database, retained the previous binary and deployed the
  tested build. This is a follow-up to the immutable snapshot already given to
  Don; a separate incremental patch and evidence accompany it.

## 2026-10-06 — Astra: public HTTPS verified

Oleg enabled Tailscale Funnel. The running service is reachable at
`https://polygon.tail42b836.ts.net`. A request forced through a public DNS
relay address returned HTTP 200 and `{"ok":true,"version":"0.2.0"}`.
The page, JS and CSS returned 200; anonymous schedule access returned 401;
the private credential-file URL returned 404. An initial account authenticated
over HTTPS with a Secure/HttpOnly/SameSite=Strict cookie, remained required to
change its password, and successfully logged out. No password was changed.
The audit snapshot previously supplied to Don remains unchanged.

## 2026-10-06 — Astra: first Polygon deployment and review

Base: `ea5dc4298b2bc2684e409e3a07d28e3837654656` (`main`).

- Reviewed the C HTTP, authentication, booking, administration, backup/restore
  and embedded Wolfe paths, plus the browser's DOM and request handling.
- Reproduced a password-policy mismatch: the API accepted six Hebrew letters
  as twelve UTF-8 bytes, while the browser required twelve characters. The
  server now enforces at least twelve Unicode code points, retaining the
  128-byte maximum. Added an HTTP regression and updated the API contract.
- Added Itzik's reusable SVG portrait to the launcher, chat heading and replies.
  The heading's expression follows pending requests and their actual outcomes.
  Added two Hebrew README passages grounded in his three tools and confirmation
  requirement. Preserved the existing layout for the interface contributors.
- On Polygon (Ubuntu 24.04, x86_64), the original 51 integration tests passed.
  After the fix, all 52 passed normally and with AddressSanitizer, leak
  detection and UndefinedBehaviorSanitizer enabled.
- Chromium browser smoke passed: login/password change, four admins, Hebrew
  proposal and explicit save, shift closure/reopening, desktop/mobile layout,
  announcement Unicode limits, logout and CSP. Inspected the mobile screenshot.
- Additional disposable-database checks passed: genuine expired cookies cannot
  read or write; HTTPS cookies carry Secure/HttpOnly/SameSite=Strict; an HTTP
  Origin downgrade is rejected. Itzik returned `no_call` for requests for coffee,
  administrator privileges and deletion of all users. A Hebrew Tuesday-morning
  request returned a proposal while the database still contained zero bookings.
- Installed dependencies in a separate user-owned directory on Polygon and
  started a persistent systemd user service with an external data directory.
  Local `/healthz` returned `{"ok":true,"version":"0.2.0"}`. External Funnel
  activation is awaiting the tailnet owner's action; public access is not yet
  verified. Initial credentials remain outside Git.
- Prepared an immutable code/patch/evidence archive for Oleg to give Don/Fable
  5.1 for independent review. No review findings from Don have arrived yet.
  Code changes are uncommitted. Raspberry Pi migration and student self-signup
  remain separate future steps.
