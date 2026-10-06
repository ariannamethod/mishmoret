# API — prototype contract

JSON requests and responses. Errors: `{"error":"code","message":"description"}`. Unsafe requests require exact configured Origin; authenticated mutations also require `X-CSRF-Token`. Credentials use an HttpOnly session cookie. Responses use no-store.

- GET `/api/session`: `{user:null}` or `{user:{id,login,name,role,must_change_password},csrf}`.
- POST `/api/login` `{login,password}`: authenticated session shape above.
- POST `/api/logout` `{}`: `{ok:true}`.
- POST `/api/password` `{old_password,new_password}`: `{ok:true,csrf}` plus a replacement session cookie; then refetch session. Rotates the current session and CSRF token atomically and revokes other sessions. New password at least 12 Unicode code points and at most 128 UTF-8 bytes, matching the browser form. Password change required before all other APIs if must_change_password=true.
- GET `/api/week?start=YYYY-MM-DD`: `{start,bookings,closures,announcements,resources}`. Sunday week start. Asia/Jerusalem dates. Each booking `{id,user_id,name,date,period,location,resource_id,resource_name,room,blocked}`. Period: morning|afternoon|full. Location: home|center. A user has at most one booking per date; POST replaces it atomically. Dates Sunday–Thursday only. All room/resource strings are plain text.
- POST `/api/wolfe` `{text,week_start}`: Itzik's embedded Wolfe inference; reads the schedule and returns `{engine:"wolfe",status,action,proposal,message}`. Text <=512 UTF-8 bytes; week_start is Sunday. Status: call|no_call|ambiguous|missing_arguments. Action: propose_booking|show_schedule|show_my_bookings or null. Booking proposal: `{date,period,location,resource_id:null}`; nothing is saved until the user submits `/api/bookings`. Named weekdays refer to the selected week; next week adds seven days; today/tomorrow use Asia/Jerusalem's current date. Missing/conflicting fields return a clarification; past/closed shifts return 409. Same session, Origin, CSRF and password-change requirements as booking. Wolfe is loaded once from `web/../wolfe/`, in neural mode; HTTP callbacks serialize calls.
- POST `/api/bookings` `{date,period,location,resource_id:null|integer}`: `{ok:true}`. Own booking only. Center resource optional. Home must use null. Full overlaps both halves.
- DELETE `/api/bookings?id=N`: `{ok:true}`; owner or admin.
- GET `/api/admin/users`: `{users:[{id,login,name,role,active,must_change_password}]}`. role admin|member.
- POST `/api/admin/users` `{login,name,role}`: `{user:{id,login,name,role},temporary_password}`. Password generated server-side and shown once. No signup endpoint.
- PATCH `/api/admin/users` `{id,name,role,active:boolean}`: `{ok:true}`. Last active administrator protected.
- POST `/api/admin/reset-password` `{id}`: `{temporary_password}`. Revokes sessions; requires password change.
- POST `/api/admin/closures` `{date,period,location,reason}`: `{ok:true}`. location all|home|center. Close whole day using full/all. Existing bookings stay stored but are marked blocked and displayed as cancelled while closure applies. Reopening restores them.
- DELETE `/api/admin/closures?id=N`: `{ok:true}`. Closure rows `{id,date,period,location,reason}`.
- POST `/api/admin/announcements` `{date,end_date,title,body}`: `{ok:true}`. All dates inclusive, title <=120 Unicode code points / 480 UTF-8 bytes, body <=1000 code points / 4000 bytes; UI may use one day for both dates.
- DELETE `/api/admin/announcements?id=N`: `{ok:true}`. Announcement rows `{id,date,end_date,title,body}`.
- POST `/api/admin/resources` `{name,room,kind}`: `{ok:true}`. kind desk|room. A room resource conflicts with every desk in the identical room string; different desks can coexist. Resource rows `{id,name,room,kind,active}`.
- PATCH `/api/admin/resources` `{id,active:boolean}`: `{ok:true}`. Disable forbidden if future bookings exist.

The browser keeps one unfinished Wolfe request in memory after `missing_arguments`.
It first interprets the next message independently. Only a `no_call` response
triggers a retry containing the unfinished text and the reply (still capped at
512 UTF-8 bytes). A recognized new command takes precedence. Session/week changes,
manual form actions and errors clear that context; the server stays stateless.
A combined reply is retained only when it reduces the missing-field set; conflicting/stalled context is discarded. While location is missing, the narrow reply `לא, מהבית` (or the corresponding center form) supplies that location. Other negations are interpreted unchanged. Single missing fields receive a specific Hebrew question. The internal location
value remains `center`; the interface displays `בחממה`.

Static files: `/`, `/app.js`, `/style.css`, `/itzik.svg`. GET `/healthz` returns `{ok:true,version}`. Default port 8080, loopback only. All application actions relative /api paths. UI must use textContent / safe DOM APIs for stored content, no inline scripts/styles or external CDNs. No data mocked in frontend.

Login and password verification use per-source KDF token buckets: burst 20,
refill one token per five seconds; a password change costs two. IPv4 addresses
are separate; IPv6 addresses share their /64. The bounded 1024-entry table never
evicts an active bucket. These counters live in process memory and reset on
restart. There is no account lock based on failures. Legacy fail_count/fail_since
columns remain for backup/schema compatibility; authentication no longer reads
or updates them. HTTP 429 means
that source must wait; users behind the same NAT share this limit.

By default source means the TCP peer and forwarding headers are ignored.
`--trust-tailscale-proxy` opts into a single validated X-Forwarded-For address
from a loopback peer. Use only behind Tailscale Serve/Funnel, which overwrites
that header; an arbitrary appending reverse proxy does not satisfy this contract.
The source limiter prevents one address from consuming everybody's KDF quota;
it does not provide distributed denial-of-service protection.
