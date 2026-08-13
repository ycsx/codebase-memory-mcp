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
