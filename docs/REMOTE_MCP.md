# 远程 MCP 部署

标准 headless 二进制可以通过 MCP Streamable HTTP 向 Codex 或其他客户端提供图谱工具。一键部署使用带嵌入式前端的 UI Release：MCP 和可视化控制台分别运行在两个 loopback 服务中，再由同一个 Nginx HTTP 入口分流。TLS 应由公司的统一反向代理终止。

## 本机管理接口（W5）

嵌入式 UI 服务另外提供一组仅供本机控制台使用的管理接口：`GET /healthz`、`GET /readyz`、`GET /metrics`，以及项目和任务目录 `GET /admin/v1/projects`、`GET /admin/v1/jobs`。索引操作可通过 `POST /admin/v1/index` 或 `POST /admin/v1/remote-index` 提交；这些接口复用控制台已有的索引队列和去重逻辑。

管理接口跟随 UI 服务绑定到 `127.0.0.1`，不提供远程 MCP 的 `/mcp` 端口，也没有跨主机鉴权语义。一键部署的 Nginx 会显式阻断 `/admin/v1/*`；不要通过其他反向代理或端口转发绕过此限制。

## Linux 一键部署（Ubuntu/Debian）

仓库提供面向单节点 Ubuntu/Debian 服务器的部署脚本。它会安装并校验带 UI 的 Release 二进制、创建独立服务账号和持久化目录、初始化托管 Key Store 和网页密码，并生成 MCP/UI 两个 systemd unit、Nginx 和审计日志轮转配置。两个后端都固定监听 loopback；对外入口默认也只监听 `127.0.0.1:9766`，脚本不生成或管理证书。

公司部署默认不需要填写参数：

```bash
sudo bash /tmp/install-codebase-memory-server.sh
```

公司统一 Nginx、网关或负载均衡器使用公司的可信证书对外提供 HTTPS，然后转发到同机 `http://127.0.0.1:9766`。外层代理必须保留 `Authorization` 请求头。服务端入口仍会用网页密码和 MCP Bearer Key 分别鉴权。

公司 Nginx 的最小上游配置示例：

```nginx
server {
    listen 443 ssl;
    server_name codebase-memory-mcp.internal;

    # ssl_certificate / ssl_certificate_key 使用公司的统一配置。
    location / {
        proxy_pass http://127.0.0.1:9766;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header Authorization $http_authorization;
        proxy_set_header X-Forwarded-For $remote_addr;
        proxy_read_timeout 180s;
        proxy_buffering off;
    }
}
```

需要临时从其他机器通过 IP 测试时，显式启用 `--public`：

```bash
curl -fsSL https://raw.githubusercontent.com/ycsx/codebase-memory-mcp/main/deploy/install-server.sh \
  -o /tmp/install-codebase-memory-server.sh
sudo bash /tmp/install-codebase-memory-server.sh \
  --public \
  --allow-cidr 10.0.0.0/8
```

`--public` 使用明文 HTTP，网页密码和 Bearer Key 在传输中没有加密，只适合受控网络里的临时测试。不要把该模式直接暴露到互联网。脚本不会修改防火墙；可使用一个或多个 `--allow-cidr` 进一步限制 MCP 请求来源。

脚本可重复执行：已有 Key Store 和网页密码会被保留，只有第一次部署才创建管理员 Key 和 `admin` 网页密码。两种明文凭据都只显示一次，应立即保存到密码管理器。脚本会先解析目标 Release，比较已安装版本并探测其嵌入式 UI；版本相同且 UI 可用时跳过二进制压缩包下载和覆盖，后续服务配置仍会正常校准。新版脚本会删除旧版脚本在 `/etc/codebase-memory-mcp/tls/` 生成的固定名称自签名证书；不会修改公司代理管理的证书。通过 `--version vX.Y.Z` 可以固定 Release，通过 `--binary /path/to/codebase-memory-mcp` 可以强制安装尚未发布的可信本地 UI 构建。完整参数见 `bash deploy/install-server.sh --help`。

远端索引会保留用户输入的协议。公开仓库建议直接使用 HTTPS 地址，不需要 SSH 配置。部署脚本会为服务账号生成独立的无口令 Ed25519 部署密钥；私有仓库使用 SSH 地址时，先执行下面的命令并把输出公钥添加为仓库只读 Deploy Key：

```bash
sudo cbm-server git-key
```

SSH 首次连接使用 `StrictHostKeyChecking=accept-new` 记录主机密钥，后续密钥变化仍会阻断连接。部署密钥只属于 `codebase-memory-mcp` 服务账号，不会复制或读取 `root` 及登录用户的私钥。

部署后的两个入口共用一个 HTTP 端口，但认证方式彼此独立：

- 默认 `http://127.0.0.1:9766/`：可视化控制台上游地址，使用安装输出的 `admin` 用户名和网页密码。
- 默认 `http://127.0.0.1:9766/mcp`：MCP 上游地址，使用托管 Bearer Key。
- `--public` 模式将上述地址改为服务器 IP，协议仍为 HTTP。

控制台入口始终要求随机生成的网页密码，MCP 入口始终要求 Bearer Key。两者默认都不限制客户端 IP；显式传入一个或多个 `--allow-cidr` 时，只为 MCP 入口增加 IP 白名单。防火墙仍可按实际网络拓扑限制 `9766`，但部署脚本不会猜测哪些地址属于你的内网。

部署完成后会注册 `cbm-server` 管理命令，常用操作不再需要填写二进制和 Key Store 的完整路径：

```bash
sudo cbm-server keys
sudo cbm-server new-key alice project-a,project-b
sudo cbm-server new-key build-bot project-a ci
sudo cbm-server rotate-key <key-id>
sudo cbm-server revoke-key <key-id>
sudo cbm-server status
sudo cbm-server logs 200
sudo cbm-server ui-status
sudo cbm-server ui-logs 200
sudo cbm-server ui-password-reset
sudo cbm-server allowed-root
sudo cbm-server set-allowed-root /data/projects
sudo cbm-server restart
```

`new-key` 和 `rotate-key` 会在终端显示一次新明文 Key；`keys` 只展示 Key ID、主体、权限和状态。由于服务端只保存哈希，任何命令都不能恢复旧明文 Key。网页密码同样只保存 bcrypt 哈希，遗失后用 `ui-password-reset` 生成新密码。完整的 `key create/list/rotate/revoke` 分组形式也继续支持。

### 本地仓库目录

浏览器中的“本地目录”指服务器文件系统，不是访问控制台的电脑。默认允许根为 `/var/lib/codebase-memory-mcp/repos`，控制台的目录选择器会锁定在该目录及其子目录，避免选中后才收到 `outside the allowed root`。如果服务器源码统一放在其他目录，可一次切换：

```bash
sudo cbm-server set-allowed-root /data/projects
sudo cbm-server allowed-root
```

切换命令会验证服务账号的读取与遍历权限、更新 `CBM_ALLOWED_ROOT` 和两个 systemd 服务的路径沙箱，然后重启 MCP 与可视化控制台。目录本身必须已存在；如果权限校验失败，应先为 `codebase-memory-mcp` 服务账号授予只读访问。Git 远程仓库仍由服务托管在状态目录中，不需要手工放入这个本地目录。

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
