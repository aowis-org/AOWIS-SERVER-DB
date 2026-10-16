# V24 — Controlled remote deployment boundary

Qt HTTP Server is **not** suitable for direct internet exposure. The server
remains bound to `127.0.0.1`, and HTTP is intentionally unencrypted **only
within that loopback boundary**. For remote access, deploy a maintained
reverse proxy on the **same host**, terminate TLS there, and expose only the
proxy. Do not proxy plain HTTP across hosts or an untrusted network. Configure
firewall rules to deny outside access to port 8080.

## Built-in safeguards

- All endpoints continue using database-backed bearer sessions and the existing
  DB-core authorization. Neither `X-Forwarded-User`, `X-Remote-User`, nor URL
  parameters select the actor. Only a precisely formatted bearer header is
  accepted by the server executable (64 lowercase hexadecimal token bytes).
- Routed API responses have `Cache-Control: no-store`, `Pragma: no-cache`,
  `X-Content-Type-Options: nosniff`, and `Referrer-Policy: no-referrer` on
  Qt 6.8+. No permissive CORS headers are supplied.
- Qt 6.9+: limit to 120 requests/second **per loopback address**, aggregated
  across reverse-proxy clients; not a substitute for per-client proxy limits.
- Qt 6.10+: keep-alive idle timeout is 10 seconds.
- Qt 6.11+: reject request bodies larger than 8 MiB, URLs longer than 8192
  bytes, headers exceeding 16 KiB total or 4096 bytes per field, and more
  than 64 header fields. Earlier versions still enforce bounded JSON bodies
  in each route, but **do not enforce a parser-level request-body cap**.
- Project records, bearer secrets, and error details are not added to the
  health check. Administrative bootstrap and token issuance remain local-only.

## Reverse proxy (example; adjust for deployment)

Configure your TLS certificate and renewal externally. Put this snippet in nginx's `http` context (or put `limit_req_zone` in the `http` context and the `server` block in a site configuration):

```nginx
limit_req_zone $binary_remote_addr zone=aowis_per_client:10m rate=10r/s;

server {
    listen 443 ssl;
    server_name db.example.org;
    ssl_certificate /etc/letsencrypt/live/db.example.org/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/db.example.org/privkey.pem;

    client_max_body_size 8m;
    client_body_timeout 15s;
    client_header_timeout 10s;
    keepalive_timeout 10s;

    location /api/v1/ {
        limit_req zone=aowis_per_client burst=20 nodelay;
        proxy_pass http://127.0.0.1:8080;
        proxy_http_version 1.1;
        proxy_request_buffering on;
        proxy_connect_timeout 5s;
        proxy_send_timeout 20s;
        proxy_read_timeout 20s;
        proxy_set_header Host $host;
        proxy_set_header Authorization $http_authorization;
        proxy_set_header Connection "";
    }
}
```

**This example is not a turnkey deployment.** Apply operating-system and
service isolation, backups (including SQLite WAL consistency), certificate
maintenance, firewall rules, logging without token values, security updates,
monitoring, and resource limits. Do not allow untrusted client-supplied proxy
identity headers to become credentials in future middleware. Clients must
authenticate using their own bearer sessions.

AOWIS still lacks password/OIDC login, user-driven session refresh, secure
remote enrollment, credential recovery, and tested internet-operational
procedures. V24 is defense in depth for controlled deployments, not a claim
of production-ready remote authentication or exposure.
