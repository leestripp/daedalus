# Network Tools

daedalus has first-class network tools backed by [OmniNet](https://gitlab.com/leestripp/omninet) (C++23 HTTP/WebSocket library). They replace Bash `curl`, `python3 -c "import urllib"`, and `nc` for typical agent workflows.

All tools are registered automatically — the LLM sees them in the system prompt's `## Tools Available to You` section.

---

## OmniNet setup

OmniNet keeps its runtime config/data in an XDG-compliant location:

- Config: `$XDG_CONFIG_HOME/omninet/config.json` (default `~/.config/omninet/config.json`)
- Data:   `$XDG_DATA_HOME/omninet` (default `~/.local/share/omninet`)

If you have an existing OmniNet install that still uses `~/.omninet/`, daedalus honours that legacy location for one release cycle, but new installs should use the XDG paths above.

When `Agent::initialize()` runs and neither the XDG nor the legacy OmniNet directory exists, the library emits a single warning through `AgentSink::on_log`:

```
[OMNINET] OmniNet runtime config not found at <path>. Some network/WebSocket
features may be unavailable. Run 'omninet-init' or create <dir> to set it up.
```

This is not a fatal error — basic HTTP/TLS operations still work using the system CA store — but some OmniNet-specific features (e.g. server tooling, certificate management) need the directory to be initialized.

---

| Tool | Replaces | Backed by |
|---|---|---|
| `http_request` | Bash `curl` / `python3 urllib` | OmniNet TLS (system CA) + plain TCP |
| `dns_lookup` | Bash `dig` / `getent hosts` | POSIX `getaddrinfo()` |
| `tcp_check` | Bash `nc -zv` | OmniNet `Socket` |
| `tls_inspect` | Bash `openssl s_client` | OmniNet `TLSSocket` + OpenSSL X509 |
| `websocket_connect` | (none — new capability) | OmniNet `WebSocket::Connection` |
| `websocket_send` | (none) | OmniNet `WebSocket::Connection` |
| `websocket_recv` | (none) | OmniNet `WebSocket::Connection` |
| `websocket_close` | (none) | OmniNet `WebSocket::Connection` |

---

## When to prefer which tool

| Task | Use |
|---|---|
| One-shot REST call | `http_request` |
| Verify DNS before connecting | `dns_lookup` |
| Verify a service is up before hitting it | `tcp_check` |
| Debug "why is HTTPS failing?" | `tls_inspect` |
| Long-lived stream (chat, live data, pub/sub) | `websocket_*` |
| Anything else network-y | `http_request` first |

**Don't** reach for `execute_bash` to run `curl` or `python3 -c "import urllib..."`. The network tools are faster, safer (proper TLS verification), and don't eat tokens on boilerplate.

---

## `http_request`

```json
{
  "url": "https://api.github.com/repos/leestripp/daedalus",
  "method": "GET",
  "headers": {
    "Accept": "application/json"
  },
  "timeout_seconds": 30,
  "max_response_bytes": 5242880
}
```

Response on success:
```json
{
  "status": "success",
  "status_code": 200,
  "status_text": "OK",
  "headers": {
    "content-type": "application/json; charset=utf-8",
    "x-ratelimit-remaining": "53",
    ...
  },
  "body": "{\"stargazers_count\": 42, ...}",
  "body_truncated": false,
  "body_bytes": 1234,
  "final_url": "https://api.github.com:443/repos/leestripp/daedalus",
  "elapsed_ms": 187
}
```

On error:
```json
{
  "status": "error",
  "error": "connect failed: Connection refused",
  "final_url": "https://api.github.com:443/...",
  "elapsed_ms": 42
}
```

| Arg | Type | Default | Notes |
|---|---|---|---|
| `url` | string | (required) | Only `http://` and `https://` accepted. userinfo rejected. |
| `method` | string | `"GET"` | GET, POST, PUT, DELETE, PATCH, HEAD, OPTIONS |
| `headers` | object | `{}` | Additional request headers |
| `body` | string | `""` | Request body (string) |
| `timeout_seconds` | int | 30 | Min 1, max 300 |
| `insecure` | bool | `false` | Skip TLS verification (logs `[WARN]`) |
| `max_response_bytes` | int | 5242880 | Min 1024, max 52428800 (50 MB) |

### Notes
- HTTP/1.1 only. The tool sets `ALPN: http/1.1` to avoid HTTP/2 negotiation on servers that prefer h2.
- TLS verification on by default. The system CA bundle (`/etc/ssl/certs/ca-certificates.crt` on Debian/Ubuntu) is loaded automatically.
- No automatic redirect following. A 3xx response is returned as-is — handle `Location` header manually if needed.
- `insecure: true` skips verification (prints `[WARN]` to stderr). Use only for self-signed test servers.

---

## `dns_lookup`

```json
{
  "hostname": "api.github.com",
  "record_type": "ANY"
}
```

Response:
```json
{
  "status": "success",
  "hostname": "api.github.com",
  "record_type": "ANY",
  "records": [
    { "family": "IPv4", "address": "140.82.121.6",  "canonical_name": "api.github.com" }
  ],
  "count": 1,
  "v4_count": 1,
  "v6_count": 0
}
```

| Arg | Type | Default | Notes |
|---|---|---|---|
| `hostname` | string | (required) | No scheme or path |
| `record_type` | enum | `"A"` | `A` (IPv4 only), `AAAA` (IPv6 only), `ANY` (both) |

Uses POSIX `getaddrinfo()` — respects `/etc/hosts`, `/etc/resolv.conf`, and NSS configuration. No network connection beyond the resolver.

---

## `tcp_check`

```json
{
  "host": "api.github.com",
  "port": 443,
  "timeout_seconds": 5
}
```

Response on success:
```json
{
  "status": "success",
  "reachable": true,
  "host": "api.github.com",
  "port": 443,
  "elapsed_ms": 87
}
```

On failure:
```json
{
  "status": "error",
  "reachable": false,
  "host": "127.0.0.1",
  "port": 1,
  "error": "connect failed: Connection refused",
  "elapsed_ms": 2
}
```

| Arg | Type | Default | Notes |
|---|---|---|---|
| `host` | string | (required) | Hostname or IP |
| `port` | int | (required) | 1-65535 |
| `timeout_seconds` | int | 5 | Min 1, max 60 |

Closes the socket immediately on success — no data is exchanged.

---

## `tls_inspect`

```json
{
  "host": "api.github.com",
  "port": 443,
  "timeout_seconds": 10
}
```

Response:
```json
{
  "status": "success",
  "host": "api.github.com",
  "port": 443,
  "tls_version": "TLSv1.3",
  "cipher": "TLS_AES_128_GCM_SHA256",
  "alpn_protocol": "",
  "peer_cert_verified": true,
  "verify_result": "ok",
  "certificate": {
    "present": true,
    "subject": "/CN=*.github.com",
    "issuer": "/C=GB/O=Sectigo Limited/CN=Sectigo Public Server Authentication CA DV E36",
    "not_before": "2026-05-04T00:00:00Z",
    "not_after":  "2026-08-01T23:59:59Z",
    "days_until_expiry": 36,
    "signature_algorithm": "ecdsa-with-SHA256",
    "public_key": { "algorithm": "EC", "size_bits": 256 },
    "subject_alt_names": [
      { "type": "DNS", "value": "*.github.com" },
      { "type": "DNS", "value": "github.com" }
    ],
    "fingerprint_sha256": "4FEDE9C4EF53E3DDEEC6FEC3F31CE0CF918768312A82EA405CA55B88446DB8C7"
  },
  "elapsed_ms": 87
}
```

| Arg | Type | Default | Notes |
|---|---|---|---|
| `host` | string | (required) | |
| `port` | int | 443 | 1-65535 |
| `timeout_seconds` | int | 10 | Min 1, max 60 |
| `insecure` | bool | `false` | Skip TLS verification (logs `[WARN]`) |

Only does the TLS handshake — no HTTP request is sent. Useful for:
- "Is this HTTPS cert about to expire?"
- "What cipher is the server negotiating?"
- "Why is my HTTPS call failing — bad cert, wrong hostname, or untrusted CA?"

---

## `websocket_*` (long-lived connections)

WebSocket connections are **stateful**. The four tools share a process-singleton `WsRegistry` that holds the live connections, keyed by string IDs like `ws_0x1`, `ws_0x2`, etc.

### Typical flow

```jsonc
// 1. Open the connection
{"tool": "websocket_connect", "args": {
  "url": "ws://echo.websocket.events"
}}
// → {"connection_id": "ws_0x1", ...}

// 2. Send a message
{"tool": "websocket_send", "args": {
  "connection_id": "ws_0x1",
  "message": "hello"
}}

// 3. Receive the response (with a timeout)
{"tool": "websocket_recv", "args": {
  "connection_id": "ws_0x1",
  "timeout_seconds": 10
}}
// → {"status": "success", "type": "text", "payload": "hello", ...}

// 4. Close cleanly
{"tool": "websocket_close", "args": {
  "connection_id": "ws_0x1",
  "code": 1000,
  "reason": "done"
}}
```

### `websocket_connect`

| Arg | Type | Default | Notes |
|---|---|---|---|
| `url` | string | (required) | `ws://` only — `wss://` is rejected (OmniNet 0.7.4 client doesn't support TLS yet) |
| `protocol` | string | `""` | Optional `Sec-WebSocket-Protocol` |
| `timeout_seconds` | int | 10 | Min 1, max 60 |

### `websocket_send`

Exactly one of:

| Arg | Type | Notes |
|---|---|---|
| `connection_id` | string | (required) From `websocket_connect` |
| `message` | string | Text payload (UTF-8) |
| `message_base64` | string | Binary payload, base64-encoded |

Response: `{status, connection_id, bytes_sent, type: "text"|"binary"}`

### `websocket_recv`

| Arg | Type | Default | Notes |
|---|---|---|---|
| `connection_id` | string | (required) | |
| `timeout_seconds` | int | 30 | Min 1, max 300. Bounds how long the tool waits. |

Response:
```json
{
  "status": "success",
  "connection_id": "ws_0x1",
  "type": "text",
  "payload": "the message"
}
```
or for binary:
```json
{
  "status": "success",
  "connection_id": "ws_0x1",
  "type": "binary",
  "payload_base64": "ZGVhZGJlZWY=",
  "bytes": 8
}
```

When the peer closes the connection cleanly, you get `{status: "closed"}` instead of `success`.

### `websocket_close`

| Arg | Type | Default | Notes |
|---|---|---|---|
| `connection_id` | string | (required) | |
| `code` | int | 1000 | 1000=normal, 1001=going away, 1002=protocol error, 1003=unsupported data, 1008=policy violation, 1011=internal error |
| `reason` | string | `""` | Human-readable close reason |

### Connection lifecycle

- **Idle eviction**: connections idle for more than 5 minutes are closed automatically by a background reaper thread.
- **Lost on restart**: connection IDs are in-memory only. After restarting daedalus, the LLM must reconnect.
- **One process**: the registry is process-local. Don't share connection IDs across multiple daedalus instances.

### Known limitation (OmniNet 0.7.4)

- `wss://` is **not** supported yet. OmniNet's client-side `Connection(url)` ctor always uses a plain (unencrypted) Socket. The daedalus tool rejects `wss://` with a clear error rather than hanging. Tracked for upstream fix.

---

## Examples

### Fetch a public API

```jsonc
{"tool": "http_request", "args": {
  "url": "https://api.github.com/repos/leestripp/daedalus",
  "method": "GET",
  "headers": {"Accept": "application/vnd.github+json"}
}}
```

### POST JSON

```jsonc
{"tool": "http_request", "args": {
  "url": "https://httpbin.org/post",
  "method": "POST",
  "headers": {"Content-Type": "application/json"},
  "body": "{\"hello\":\"world\"}"
}}
```

### Verify DNS then make a request

```jsonc
{"tool": "dns_lookup", "args": {"hostname": "api.github.com", "record_type": "ANY"}}
// If you got records back:
{"tool": "http_request", "args": {"url": "https://api.github.com/zen"}}
```

### Debug an HTTPS failure

```jsonc
// Step 1: confirm the service is up
{"tool": "tcp_check", "args": {"host": "api.example.com", "port": 443}}

// Step 2: inspect the cert (was it expired? wrong CA?)
{"tool": "tls_inspect", "args": {"host": "api.example.com", "port": 443}}

// Step 3: if cert looks fine, try the actual request
{"tool": "http_request", "args": {"url": "https://api.example.com/v1/users"}}
```

### WebSocket round trip

```jsonc
{"tool": "websocket_connect", "args": {"url": "ws://echo.websocket.events"}}
// → connection_id: "ws_0x1"

{"tool": "websocket_send", "args": {
  "connection_id": "ws_0x1",
  "message": "hello websocket"
}}

{"tool": "websocket_recv", "args": {
  "connection_id": "ws_0x1",
  "timeout_seconds": 5
}}
// → {"type": "text", "payload": "hello websocket", ...}

{"tool": "websocket_close", "args": {"connection_id": "ws_0x1"}}
```

---

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `http_request` returns `error: "connect failed"` | Service down or firewalled | Run `tcp_check` first |
| `http_request` hangs past `timeout_seconds` | Server slow or large body | Increase `timeout_seconds`, decrease `max_response_bytes` |
| `http_request` returns `peer_cert_verified: false` | Bad cert chain | Run `tls_inspect` to see why |
| `http_request` returns `getaddrinfo failed` | DNS issue | Run `dns_lookup` to debug |
| `websocket_recv` returns `closed` | Server closed connection (or your `close()` was called too eagerly) | Check connection state with a small `timeout_seconds` |
| `websocket_connect` rejects `wss://` | OmniNet 0.7.4 client TLS not implemented | Use a non-TLS endpoint for now, or wait for upstream fix |

---

## See also

- [OmniNet project](https://gitlab.com/leestripp/omninet) — the C++23 HTTP/WebSocket library backing these tools
- [tools.md](tools.md) — overview of all daedalus tools
- [skills.md](skills.md) — when to load a skill
