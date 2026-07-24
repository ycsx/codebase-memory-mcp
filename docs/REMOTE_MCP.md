# Remote MCP deployment

The standard headless binary can expose the graph tools to Codex over MCP
Streamable HTTP. Embedded UI assets are not required and the local graph UI
remains bound to loopback.

## Security model

- Endpoint: `POST /mcp` with optional `DELETE /mcp` session teardown.
- Authentication: `Authorization: Bearer <token>` on every request.
- Tool surface: `analysis` by default; `scout` is also supported. The remote
  server refuses the unrestricted profile.
- Sessions: a separate `cbm_mcp_server_t` per `Mcp-Session-Id`, bound to the
  effective client IP and expired after the configured TTL.
- Browser requests: requests carrying `Origin` are rejected. Server-sent event
  streams are not enabled; `GET /mcp` returns `405`.
- Audit: one JSON object per request with IP, session, method/tool, sanitized
  target, status, duration, and byte counts. Tokens and full query bodies are
  never written.

IP addresses identify network origins, not people. NAT, shared workstations,
and proxies can make several users appear under one IP. The audit schema keeps
`principal` separate so per-user credentials can be added later.

## Server configuration

Create a token with at least 256 bits of entropy:

```bash
openssl rand -hex 32
```

Copy `.env.example` to a deployment-only file and replace the token. The binary
reads process environment variables; it does not automatically load `.env`.
Use systemd `EnvironmentFile=`, Docker Compose `env_file:`, or explicitly load
the file in the service launcher.

Direct intranet mode:

```bash
set -a
. ./.env
set +a
./codebase-memory-mcp serve --bind=0.0.0.0 --port=9766
```

Bearer tokens are visible to anyone who can observe unencrypted HTTP traffic.
For normal deployment, use the supplied systemd and Nginx examples: Nginx
terminates TLS on port `9766` and forwards to `127.0.0.1:19766`.

When a reverse proxy is used, set:

```dotenv
CBM_MCP_TRUSTED_PROXIES=127.0.0.1/32
```

Only trusted socket peers may supply `X-Forwarded-For`, and the proxy must
overwrite rather than append the incoming header. With an empty trusted proxy
list, the header is ignored and the socket peer is audited.

## Codex client

Make the same token available to the process that launches Codex as
`CBM_REMOTE_MCP_TOKEN`, then add:

```toml
[mcp_servers.codebase_memory_remote]
url = "https://codebase-memory-mcp.internal:9766/mcp"
bearer_token_env_var = "CBM_REMOTE_MCP_TOKEN"
startup_timeout_sec = 10
tool_timeout_sec = 180
```

Do not put the token value in `config.toml`. Restart Codex after changing its
environment or MCP configuration.

## Audit example

```json
{"ts":"2026-07-24T02:13:43Z","request_id":"fb7ecc7b32346b61","client_ip":"10.20.1.35","principal":"ip:10.20.1.35","auth_key_id":"3eb1bd439947","session_id":"7b56ffe6...","method":"tools/call","tool":"search_graph","target":"project=example","status":"ok","http_status":200,"duration_ms":42,"request_bytes":455,"response_bytes":1200}
```

Protect the audit directory so only the service account and administrators can
read it. On POSIX systems the server sets the audit file mode to `0600`.
