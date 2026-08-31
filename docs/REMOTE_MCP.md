# 远程 MCP 部署

标准 headless 二进制可以通过 MCP Streamable HTTP 向 Codex 或其他客户端提供图谱工具。嵌入式 UI 不参与远程服务，仍只绑定本机 loopback。

## 本机管理接口（W5）

嵌入式 UI 服务另外提供一组仅供本机控制台使用的管理接口：`GET /healthz`、`GET /readyz`、`GET /metrics`，以及项目和任务目录 `GET /admin/v1/projects`、`GET /admin/v1/jobs`。索引操作可通过 `POST /admin/v1/index` 或 `POST /admin/v1/remote-index` 提交；这些接口复用控制台已有的索引队列和去重逻辑。

管理接口跟随 UI 服务绑定到 `127.0.0.1`，不提供远程 MCP 的 `/mcp` 端口，也没有跨主机鉴权语义。不要通过反向代理或端口转发将 `/admin/v1/*` 暴露到网络；需要远程管理时应在受控网关上另行增加认证和授权。

## 安全模型

- 端点：`POST /mcp`，可用 `DELETE /mcp` 结束会话。
- 鉴权：每个请求都必须携带 `Authorization: Bearer <token>`。服务支持兼容模式 `CBM_MCP_AUTH_TOKEN`，也支持通过 `CBM_MCP_AUTH_STORE` 启用托管 Key Store；两者不能同时作为同一请求的鉴权来源。
- 托管 Key：服务端只保存 SHA-256 哈希，不保存明文；Key 具有 `user`、`ci` 或 `admin` 主体、`analysis`/`scout` 档位和项目 ACL。`source_read`、`index`、`delete`、`admin` 是独立权限，撤销或轮换会立即使旧 Key 失效。
- 工具档位：默认 `analysis`，也可使用 `scout`；远程服务拒绝无限制的 `all` 档位。
- 会话：每个 `Mcp-Session-Id` 使用独立 MCP server，绑定有效客户端 IP，并按 TTL 过期。
- 浏览器请求：带 `Origin` 的请求会被拒绝；不启用 SSE，`GET /mcp` 返回 `405`。
- 审计：每个请求记录 IP、会话、方法/工具、脱敏目标、状态、耗时和字节数；不会记录 Token 和完整查询正文。

IP 只代表网络来源，不一定对应单个用户。NAT、共享工作站和代理可能让多个用户使用同一个 IP；审计中的 `principal` 为后续用户级凭据保留字段。

## 服务配置

### 兼容 Token 模式

生成至少 256 bit 熵的 Token：

```bash
openssl rand -hex 32
```

复制 `.env.example` 为仅部署使用的环境文件，并设置 Token。二进制读取进程环境，不会自动加载 `.env`；可通过 systemd `EnvironmentFile=`、Docker Compose `env_file:` 或启动脚本加载。

### 托管 Key Store 模式

先创建一个 Key Store 和管理员 Key：

```bash
codebase-memory-mcp remote-key create --store=/var/lib/codebase-memory-mcp/remote-auth.json \
  --principal=platform-admin --kind=admin --projects='*' --source-read --index --delete --admin
```

启动服务时只需指向同一个 Store：

```dotenv
CBM_MCP_AUTH_STORE=/var/lib/codebase-memory-mcp/remote-auth.json
CBM_MCP_AUDIT_LOG=/var/log/codebase-memory-mcp/audit.jsonl
```

Key Store 文件应由服务账号拥有并限制读取权限。`remote-key list` 不会输出哈希或明文；创建和轮换返回的明文 Key 只应通过安全渠道交给客户端。

内网直连示例：

```bash
set -a
. ./.env
set +a
./codebase-memory-mcp serve --bind=0.0.0.0 --port=9766
```

生产环境应由 Nginx/其他反向代理终止 TLS，再转发到 loopback 服务。明文 HTTP 上的 Bearer Token 可被观察到，不要直接暴露在公网。

反向代理场景设置：

```dotenv
CBM_MCP_TRUSTED_PROXIES=127.0.0.1/32
```

只有可信 socket 对端可以提供 `X-Forwarded-For`；代理必须覆盖而不是追加客户端传来的同名 Header。可信代理列表为空时，审计使用 socket 对端地址。

## Codex 客户端

让启动 Codex 的进程获得 `CBM_REMOTE_MCP_TOKEN`，然后在配置中写入：

```toml
[mcp_servers.codebase_memory_remote]
url = "https://codebase-memory-mcp.internal:9766/mcp"
bearer_token_env_var = "CBM_REMOTE_MCP_TOKEN"
startup_timeout_sec = 10
tool_timeout_sec = 180
```

不要把 Token 写入 `config.toml`。修改环境变量或 MCP 配置后重启 Codex。

## 审计示例

```json
{"ts":"2026-07-24T02:13:43Z","request_id":"fb7ecc7b32346b61","client_ip":"10.20.1.35","principal":"ip:10.20.1.35","auth_key_id":"3eb1bd439947","session_id":"7b56ffe6...","method":"tools/call","tool":"search_graph","target":"project=example","status":"ok","http_status":200,"duration_ms":42,"request_bytes":455,"response_bytes":1200}
```

审计目录应仅允许服务账号和管理员读取；POSIX 服务会把审计文件权限设置为 `0600`。
