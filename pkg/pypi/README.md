# codebase-memory-mcp

面向 AI 编码 Agent 的本地代码知识图谱 MCP 服务。索引、查询和语义搜索均在本机执行，不需要 API Key、Docker 或语言运行时。

## 安装

```bash
pip install codebase-memory-mcp
# 或
pipx install codebase-memory-mcp
```

该包会从 [GitHub Releases](https://github.com/ycsx/codebase-memory-mcp/releases) 下载并缓存当前平台的二进制。安装后运行：

```bash
codebase-memory-mcp install
codebase-memory-mcp --help
```

## 支持平台

| 系统 | 架构 |
|---|---|
| macOS | arm64、amd64 |
| Linux | arm64、amd64 |
| Windows | amd64、arm64（以最新 Release 资产为准） |

修改客户端配置后请重启 AI 客户端。完整安装、MCP 工具、配置和远程部署说明见：

[项目 README](https://github.com/ycsx/codebase-memory-mcp#readme)

## 文档与代码引用

当前工作区源码定义 20 个 MCP 工具（`all` 档位），其中 `analysis` 为 16 个、
`scout` 为 10 个；权限控制可进一步缩小实际工具列表。
`get_document` 提供 Markdown 文档到文件/完整限定名符号的显式引用和行号证据，
`get_related_documents` 支持反向查询；`build_context`、`explain_impact`、
`review_change` 可按需返回有数量与预算限制的相关文档。

本轮未核验 PyPI 或 GitHub Release 是否已经包含这些源码改动。使用包含改动的构建，
确认客户端指向该二进制、重启 MCP/AI 客户端，并对已有项目重新索引后生效。
功能可通过本地 stdio/CLI 使用，不依赖 HTTP 部署；空结果不是文档覆盖完整的证明。

支持语法、参数和结果限制见[文档引用使用指引](https://github.com/ycsx/codebase-memory-mcp/blob/main/docs/DOCUMENT_REFERENCES.md)。
