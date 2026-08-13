# 配置参考

## 文件位置

| 用途 | 路径 | 格式 |
|---|---|---|
| 全局扩展名映射 | `$XDG_CONFIG_HOME/codebase-memory-mcp/config.json`，未设置时为 `~/.config/codebase-memory-mcp/config.json` | JSON |
| 项目扩展名映射 | `{repo_root}/.codebase-memory.json` | JSON |
| CLI 运行时设置 | `${CBM_CACHE_DIR:-~/.cache/codebase-memory-mcp}/_config.db` | SQLite |
| UI 设置 | `${CBM_CACHE_DIR:-~/.cache/codebase-memory-mcp}/config.json` | JSON |

## 扩展名映射

全局配置和仓库根目录的 `.codebase-memory.json` 都支持：

```json
{
  "extra_extensions": {
    ".blade.php": "php",
    ".mjs": "javascript",
    ".twig": "html"
  }
}
```

扩展名必须带点，语言名不区分大小写；未知语言和缺失文件会被忽略。项目级配置优先于全局配置。

## CLI 设置

```bash
codebase-memory-mcp config list
codebase-memory-mcp config get auto_index
codebase-memory-mcp config set auto_index true
codebase-memory-mcp config set auto_index_limit 50000
codebase-memory-mcp config set auto_watch false
codebase-memory-mcp config reset auto_index
```

| 键 | 默认值 | 说明 |
|---|---|---|
| `auto_index` | `false` | MCP 初始化时是否自动索引新项目。 |
| `auto_index_limit` | `50000` | 自动索引允许的最大文件数。 |
| `auto_watch` | `true` | 已索引项目是否注册后台 watcher。 |

## UI 设置

`${CBM_CACHE_DIR:-~/.cache/codebase-memory-mcp}/config.json`：

```json
{
  "ui_enabled": false,
  "ui_port": 9749,
  "lang": "zh"
}
```

UI 版本在首次运行时可自动启用嵌入资源。 `CBM_CACHE_DIR` 同时影响索引数据库、CLI 设置和 UI 配置。

## 环境变量

| 变量 | 默认值 | 说明 |
|---|---|---|
| `CBM_ALLOWED_ROOT` | 未设置 | 将 MCP 和 UI 索引限制在指定目录内。 |
| `CBM_CACHE_DIR` | `~/.cache/codebase-memory-mcp` | 覆盖缓存、索引和配置目录。 |
| `CBM_DIAGNOSTICS` | `false` | 输出周期性诊断文件。 |
| `CBM_DOWNLOAD_URL` | GitHub Releases | 覆盖升级下载地址。 |
| `CBM_LOG_LEVEL` | `info` | `debug`、`info`、`warn`、`error`、`none` 或 `0`-`4`。日志写 stderr。 |
| `CBM_WORKERS` | 自动检测 | 覆盖索引 worker 数，范围 1-256。 |
| `CBM_MEM_BUDGET_MB` | 自动检测 | 覆盖内存预算，单位 MiB。 |
| `CBM_DUMP_VERIFY_MIN_RATIO` | `0.5` | 校验持久化节点数，低于比例时报告 degraded；设置 `0` 可关闭。 |

## 客户端接入文件

`install` 可写入 Claude Code、Codex、Gemini、VS Code、Cursor、Zed 等客户端的 MCP、说明、技能、Agent 和生命周期 Hook。路径和客户端能力会随平台变化，建议先执行：

```bash
codebase-memory-mcp install --dry-run
codebase-memory-mcp install --plan
```

控制台的“客户端接入”面板会调用相同的计划接口，并在确认后执行写入。安装器只更新自己拥有的条目，保留无关设置，冲突时拒绝覆盖。
