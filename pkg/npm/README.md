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

当前工作区源码定义 20 个工具（`all` 档位），其中 `analysis` 为 16 个、`scout` 为 10 个；启用权限控制后实际可见工具可能更少。工具包括索引/项目管理、图谱搜索、任务上下文编译、调用链、源码读取、架构和影响分析、覆盖度检查、Cypher、ADR、运行时 trace，以及 Markdown 文档与代码的双向引用查询。

`get_document` 返回文档到代码的引用和行号证据；`get_related_documents` 从文件或完整限定名符号反查文档。`build_context`、`explain_impact` 和 `review_change` 可按需返回有数量与预算限制的相关文档证据。空结果不代表不存在相关文档，显式引用也不代表语义理解或完整覆盖。

这些描述对应工作区源码，不保证当前 npm 下载的 Release 已包含本轮功能。本轮未核验发布资产；使用包含改动的构建、重启 MCP/AI 客户端，并对已有项目重新索引后再验证。无需部署 HTTP 服务。

完整参数见仓库 [README.md](https://github.com/ycsx/codebase-memory-mcp/blob/main/README.md)，升级和查询示例见[文档引用使用指引](https://github.com/ycsx/codebase-memory-mcp/blob/main/docs/DOCUMENT_REFERENCES.md)。

## 许可证

MIT
