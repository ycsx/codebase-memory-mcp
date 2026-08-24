# codebase-memory-mcp

面向 AI 编码 Agent 的本地代码知识图谱 MCP 服务。它通过 Tree-sitter 和 Hybrid LSP 将仓库解析为函数、类、调用链、路由、配置和跨服务关系，并通过 MCP 提供结构化查询。

## 安装

```bash
npm install -g codebase-memory-mcp
codebase-memory-mcp install
```

npm 包会在安装时下载当前平台的 GitHub Release 二进制。GitHub Release 是本分支的主要发布渠道；注册表包可能滞后，请确认下载地址指向 `ycsx/codebase-memory-mcp`。

## 支持平台

| 系统 | 架构 |
|---|---|
| macOS | arm64、amd64 |
| Linux | arm64、amd64 |
| Windows | amd64、arm64（以最新 Release 资产为准） |

## 常用命令

```bash
codebase-memory-mcp --version
codebase-memory-mcp install --dry-run
codebase-memory-mcp install -y
codebase-memory-mcp config list
codebase-memory-mcp update
codebase-memory-mcp uninstall
```

重启 AI 客户端后即可使用。普通代码任务中先调用 `list_projects` 和 `index_status`，只在目标仓库尚未索引时调用 `index_repository`。

## 客户端接入

`install` 共支持 43 个自动/条件接入面：
<!-- client-surface-contract: total=43 automatic=37 conditional=6 -->

- **自动检测（37 个）**：Claude Code、Codex CLI、Gemini CLI、Zed、OpenCode、Antigravity、Aider、KiloCode、VS Code、Cursor、Windsurf、Augment / Auggie、OpenClaw、Kiro、Junie、Hermes、OpenHands、Cline、Warp、Qwen Code、GitHub Copilot CLI、Factory Droid、Crush、Goose、Mistral Vibe、Qoder CLI、Kimi Code CLI、GitLab Duo CLI、Rovo Dev CLI、Amp、Devin CLI / Local、Tabnine、Amazon Q Developer IDE、CodeBuddy Code CLI、IBM Bob Shell、Pochi、Pi。
- **条件/显式（6 个）**：Continue / cn、Visual Studio、TRAE、Roo Code、IBM Bob IDE、Sourcegraph Cody。

## CLI 模式

```bash
codebase-memory-mcp cli index_repository '{"repo_path":"/path/to/repo","mode":"moderate"}'
codebase-memory-mcp cli search_graph '{"project":"my-project","query":"order handler"}'
codebase-memory-mcp cli trace_path '{"project":"my-project","function_name":"main","direction":"both"}'
codebase-memory-mcp cli get_architecture '{"project":"my-project"}'
```

## MCP 工具

当前二进制提供 17 个工具，分为索引/项目管理、图谱搜索、任务上下文编译、调用链、源码读取、架构和影响分析、覆盖度检查、Cypher、ADR 与运行时 trace。完整参数和覆盖度边界见仓库 [README.md](https://github.com/ycsx/codebase-memory-mcp/blob/main/README.md)。

## 许可证

MIT
