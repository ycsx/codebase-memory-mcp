# codebase-memory-mcp

> 面向 AI 编码 Agent 的本地代码知识图谱 MCP 服务。它把代码库解析为可持久化、可查询的结构化图谱，让 Agent 先查符号、调用链和影响范围，再读取必要的源码。

本仓库是 `ycsx/codebase-memory-mcp` 分支，基于
[DeusData/codebase-memory-mcp](https://github.com/DeusData/codebase-memory-mcp)，增加了本地化图谱控制台、Windows/macOS Desktop 控制器、跨仓库关联、Vue 2/Webpack 分析以及发布和升级流程。

## 当前实现

- **本地优先**：索引、查询、向量语义搜索和 LSP 辅助解析均在本机执行；项目不内置大模型，也不要求 API Key、Docker 或语言运行时。
- **结构化图谱**：包含 Project、Folder、File、Module、Class、Function、Method、Interface、Route、Resource 等节点，以及 `CALLS`、`IMPORTS`、`HTTP_CALLS`、`ASYNC_CALLS`、`DATA_FLOWS` 等关系。
- **16 个 MCP 工具**：索引、项目管理、图谱搜索、调用链、代码片段、架构、影响分析、覆盖度校验、Cypher 查询、ADR 和运行时 trace 等能力。远程服务默认只开放 `analysis` 工具档位。
- **Tree-sitter + Hybrid LSP**：内置多语言、配置、模板和基础设施文件解析；对 Python、TypeScript/JavaScript/JSX/TSX、PHP、C#、Go、C/C++、Java、Kotlin、Rust、Perl 等语言提供类型和调用解析增强。具体结果以当前二进制的 `get_architecture` 和 `index_status` 为准。
- **覆盖度可审计**：索引结果会区分 `parse_partial`、`skipped` 和按规则排除的 `not_indexed` 文件。没有记录缺口不等于证明仓库完整覆盖，重要结论应使用 `check_index_coverage` 并在必要时回退源码检查。
- **可视化与桌面控制**：UI 只绑定 loopback，默认端口 `9749`，提供项目、图谱、影响、热点、文件风险、进程、日志和客户端接入计划视图。
- **多客户端接入**：`install` 会按客户端实际存在的配置和平台条件生成 MCP 配置、说明、技能、Agent 或 Hook；支持 43 个自动/条件接入面，但不会擅自开启实验开关、YOLO 权限或未确认的配置路径。

## 快速开始

### macOS / Linux

安装标准 MCP 二进制：

```bash
curl -fsSL https://raw.githubusercontent.com/ycsx/codebase-memory-mcp/main/install.sh | bash
```

安装包含图谱 UI 的版本：

```bash
curl -fsSL https://raw.githubusercontent.com/ycsx/codebase-memory-mcp/main/install.sh | bash -s -- --ui
```

脚本会按当前架构下载 GitHub Release 资产并校验 `checksums.txt`。支持 `--ui`、`--standard`、`--skip-config` 和 `--dir=<path>`。

### Windows

普通 Windows 设备请选择 AMD64，Windows on Arm 请选择 ARM64：

- [Windows AMD64 Setup](https://github.com/ycsx/codebase-memory-mcp/releases/latest/download/codebase-memory-mcp-ui-windows-amd64-setup.exe)
- [Windows ARM64 Setup](https://github.com/ycsx/codebase-memory-mcp/releases/latest/download/codebase-memory-mcp-ui-windows-arm64-setup.exe)

Setup 是按用户安装，不需要管理员权限，包含 MCP 二进制和 Desktop 控制器。脚本安装方式：

```powershell
Invoke-WebRequest https://raw.githubusercontent.com/ycsx/codebase-memory-mcp/main/install.ps1 -OutFile install.ps1
Unblock-File .\install.ps1
.\install.ps1
```

安装或修改配置后，重启 AI 客户端。首次使用可以直接告诉 Agent：`Index this project`。

### 从源码构建

需要 C/C++ 编译器、zlib 和 Git。Linux 可安装 `build-essential zlib1g-dev`，macOS 可使用 Xcode Command Line Tools。

```bash
git clone https://github.com/ycsx/codebase-memory-mcp.git
cd codebase-memory-mcp
scripts/build.sh
scripts/build.sh --with-ui
```

二进制输出为 `build/c/codebase-memory-mcp`（Windows 为 `.exe`）。构建版本可用 `scripts/build.sh --version vX.Y.Z` 写入版本号。

## 使用方式

### 索引与查询

```bash
# 查看已登记项目，project 使用返回结果中的 name
codebase-memory-mcp cli list_projects

# 索引一个仓库
codebase-memory-mcp cli index_repository '{"repo_path":"/path/to/repo","mode":"moderate"}'

# 按符号查找，再读取准确源码
codebase-memory-mcp cli search_graph '{"project":"my-project","query":"order handler"}'
codebase-memory-mcp cli get_code_snippet '{"project":"my-project","qualified_name":"..."}'

# 查询调用者和被调用者
codebase-memory-mcp cli trace_path '{"project":"my-project","function_name":"ProcessOrder","direction":"both"}'
```

索引模式：`fast` 适合快速结构索引，`moderate`（推荐）包含过滤文件和语义边，`full` 包含全部文件及相似/语义边；`cross-repo-intelligence` 用于已索引项目之间的路由和消息关联。

普通会话不要重复索引：先调用 `list_projects`，再用 `index_status` 判断状态。开启 `auto_index` 后，新项目可以在 MCP 初始化时自动索引；已索引项目仍由 watcher 负责增量更新。

### 图谱控制台

UI 版本或 Desktop 安装包可启动本地控制台：

```bash
codebase-memory-mcp console --port=9749
```

服务只监听 `127.0.0.1`，默认自动打开浏览器；CI 或无界面环境加 `--no-open`。UI 包含本地目录和 Git 远程仓库索引、增量刷新、跨仓库关联、架构/影响/热点视图、文件风险、进程监控、日志和客户端接入预览。

Windows/macOS Desktop 负责管理独立的 `console` 子进程，不会为每个 AI 会话重复创建可视化服务。外部启动且不属于 Desktop 管理的服务只展示状态，不提供停止操作。

### 客户端接入

最简单的方式是运行：

```bash
codebase-memory-mcp install --dry-run
codebase-memory-mcp install -y
```

`--dry-run`/`--plan` 只生成计划，不写文件；控制台“客户端接入”面板也会先展示待修改的配置、说明、技能和 Hook。确认后才会应用。安装器会保留无关配置，遇到无法确认归属的冲突条目会拒绝覆盖。

如果手动配置，以 Claude Code 为例：

```json
{
  "mcpServers": {
    "codebase-memory-mcp": {
      "command": "/absolute/path/to/codebase-memory-mcp",
      "args": []
    }
  }
}
```

重启客户端后检查 MCP 列表中是否出现 `codebase-memory-mcp`。若工具不可见，先重启或重新连接客户端，不要再次下载或重复安装二进制。

## MCP 工具

| 类别 | 工具 | 用途 |
|---|---|---|
| 索引 | `index_repository` | 创建或更新项目索引，也支持跨仓库智能关联和共享图谱 artifact。 |
| 项目 | `list_projects`、`delete_project`、`index_status` | 查看项目、删除项目、检查节点/边/分支和覆盖度。 |
| 发现 | `search_graph`、`search_code` | 结构化符号搜索和图谱增强的文本搜索。 |
| 关系 | `trace_path` | 追踪调用者、被调用者、数据流和跨服务链路；别名 `trace_call_path`。 |
| 源码 | `get_code_snippet` | 根据 `search_graph` 返回的限定名读取函数、类或符号源码。 |
| 分析 | `get_architecture`、`explain_impact`、`detect_changes` | 架构概览、变更影响和 Git diff 风险分析。 |
| 校验 | `check_index_coverage`、`get_graph_schema` | 检查文件/目录覆盖度和图谱 schema。 |
| 深度/记录 | `query_graph`、`manage_adr`、`ingest_traces` | 只读 Cypher 查询、架构决策记录和运行时调用 trace。 |

推荐的发现顺序：`search_graph` 找候选，`trace_path` 看关系，`get_code_snippet` 验证定义；只有需要聚合、多跳、架构或影响分析时再使用 `query_graph`、`get_architecture` 或 `explain_impact`。`search_graph` 和 `query_graph` 返回分页/行数限制时，必须检查 `has_more`、`offset` 或 `LIMIT`。

## 远程 MCP（Codex）

标准二进制可以通过带 Bearer Token 的 Streamable HTTP 对外提供 MCP：

```bash
codebase-memory-mcp serve --bind=0.0.0.0 --port=9766
```

远程模式要求设置 `CBM_MCP_AUTH_TOKEN` 和 `CBM_MCP_AUDIT_LOG`，默认使用 `analysis` 工具档位；请用 TLS 反向代理，不要把 Token 写进 Codex 的 `config.toml`。完整的 systemd、Nginx、可信代理和 Codex 配置见 [docs/REMOTE_MCP.md](docs/REMOTE_MCP.md)。

## 配置

| 配置 | 默认值 | 说明 |
|---|---|---|
| `auto_index` | `false` | MCP 会话初始化时是否自动索引新项目。 |
| `auto_index_limit` | `50000` | 自动索引允许的最大文件数。 |
| `auto_watch` | `true` | 是否把项目注册到后台增量 watcher。 |
| `CBM_CACHE_DIR` | `~/.cache/codebase-memory-mcp` | 索引、运行时设置和 UI 配置目录。 |
| `CBM_ALLOWED_ROOT` | 未设置 | 限制可索引路径的根目录。 |
| `CBM_LOG_LEVEL` | `info` | `debug`、`info`、`warn`、`error` 或 `none`。 |
| `CBM_WORKERS` | 自动检测 | 覆盖索引并行 worker 数。 |
| `CBM_MEM_BUDGET_MB` | 自动检测 | 限制内存预算。 |

常用命令：

```bash
codebase-memory-mcp config list
codebase-memory-mcp config set auto_index true
codebase-memory-mcp config set auto_watch false
codebase-memory-mcp config reset auto_index
```

扩展名映射可写入全局 `~/.config/codebase-memory-mcp/config.json` 或仓库根目录 `.codebase-memory.json`。详细路径和 JSON 格式见 [docs/CONFIGURATION.md](docs/CONFIGURATION.md)。

## 共享图谱 artifact

显式索引时可使用 `persistence=true` 生成 `.codebase-memory/graph.db.zst`，让团队成员先导入压缩图谱再做增量索引。该文件是可选的二进制产物，是否提交到 Git 由团队决定；不提交时可将 `.codebase-memory/` 加入 `.gitignore`。

## 安全与边界

- 默认只监听 stdio 或 loopback；远程 HTTP 必须显式配置 Token 和审计日志。
- 索引和 UI 有路径边界检查，不应把凭据目录、整个用户目录或系统根目录作为项目根。
- `install` 会写入 Agent 配置，这是其设计用途；先运行 `install --plan` 或 `install --dry-run` 审核目标文件。
- 图谱关系是静态分析结果。反射、动态注册、事件总线和运行时生成路径可能无法完整发现；不要把空结果当作“绝对不存在”。

## 故障排查

| 现象 | 处理 |
|---|---|
| 客户端看不到 MCP | 检查配置中的绝对路径，重启/重新连接客户端；运行 `codebase-memory-mcp --version` 验证二进制。 |
| `index_repository` 失败 | 使用绝对 `repo_path`，先检查 `CBM_ALLOWED_ROOT`、文件权限和索引状态。 |
| `trace_path` 没有结果 | 先用 `search_graph(name_pattern=".*Name.*")` 找到准确限定名。 |
| 结果可能漏项 | 调用 `index_status`/`check_index_coverage`，读取报告的范围并回退源码搜索。 |
| UI 无法打开 | 使用 UI 版本，确认端口未被占用，并访问 `http://127.0.0.1:9749`。 |

## 相关文档

- [安装与源码构建](INSTALL.md)
- [配置参考](docs/CONFIGURATION.md)
- [远程 MCP 部署](docs/REMOTE_MCP.md)
- [Desktop 控制器](desktop/README.md)
- [忽略规则](docs/cbmignore.md)
- [安全策略](SECURITY.md)
- [发布与性能资料](docs/PROJECT_REPORT.md)、[基准测试](docs/BENCHMARK.md)

## 许可证

MIT
