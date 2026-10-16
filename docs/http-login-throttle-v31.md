# V31 — Pre-KDF HTTP Login Throttling

The password-login endpoint computes an intentionally expensive PBKDF2-HMAC-SHA-256
verifier. Without an additional limit, unauthenticated clients can repeatedly
ask the server to perform expensive derivations for unknown or incorrect users.
The database's five-failure account lockout alone does not bound this work.

`HttpRoutes` now applies an in-process sliding-window limit **before** calling
`Database::loginWithPassword`:

- At most **8 attempts per normalized username per 60 seconds**.
- At most **20 total attempts per HTTP server instance per 60 seconds**.
- A syntactically valid login attempt consumes capacity whether its password
  is correct, incorrect, or empty, or the username is unknown. Requests
  rejected at JSON validation (including missing password fields) do not
  consume capacity.
- Exceeding either limit returns HTTP **429 Too Many Requests**, without a
  password derivation or an additional database mutation.
- Username buckets are indexed by SHA-256 of the normalized and case-folded
  login name. The original usernames are not kept in limiter state.
- The limiter uses a monotonic timer. Its bounded counters live in memory,
  are reset on server restart, and do **not** replace durable account lockout.
- The limiter runs on the HTTP/database thread. The existing database C++
  API is unchanged, including its PBKDF2 parameters and account lockout logic.

## Deployment trade-offs

This is deliberately a conservative, process-wide safeguard for the current
small, loopback-only service. When multiple users share one reverse proxy,
they also share its 20-attempt window. Malicious traffic can therefore cause
legitimate clients to receive 429 until the window expires. A production
multi-user deployment will need properly configured **per-client** limits at
its TLS-terminating reverse proxy, plus operational monitoring. Never trust
client-supplied forwarded IP headers directly.

The current synchronous KDF also blocks its HTTP event-loop thread while a
password is verified. This limit bounds abuse but does not make the HTTP
server capable of supporting arbitrary authentication traffic.
