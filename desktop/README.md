# 风暴之眼 Desktop

Windows/macOS 桌面服务控制器。它启动一个 `codebase-memory-mcp console` 子进程，为图谱控制台和本机 MCP 客户端提供服务。

## 功能

- 独立窗口和系统托盘。
- 启动、停止、重启 Desktop 自己管理的 MCP 可视化服务。
- 展示状态、PID、端口、运行时间、内存、实时日志和 MCP 调用统计。
- 在独立 Electron 窗口中打开图谱控制台。
- 桌面端重启后通过本地身份标记接管同一个服务 PID。
- 外部启动且不属于 Desktop 管理的服务仅展示状态，不提供停止操作。
- Windows 支持校验和验证后的 Setup 自动升级；macOS 暂不支持应用内自动升级。
- 提供“AI 接入”区域，可复制给 Agent 使用的本机 MCP 操作提示词。

## 本地运行

```powershell
cd desktop
npm install
npm start
```

开发模式按顺序查找服务二进制：

1. `CBM_BINARY`
2. Electron `resources/bin`
3. `%LOCALAPPDATA%\Programs\codebase-memory-mcp`（Windows）
4. 仓库的 `build/c/codebase-memory-mcp`（Windows 带 `.exe`）

端口读取 `~/.cache/codebase-memory-mcp/config.json` 的 `ui_port`，无有效配置时使用 `9749`。健康检查只访问 `http://127.0.0.1:<port>/api/processes`。

## 多个 Agent 共用一个服务

在桌面窗口启动服务后，Codex 和 Claude Code 已有的默认本机 MCP 配置会自动切换到同一端口的 `/mcp`。已打开的客户端需要重新连接或重启才能读取新配置。旧 stdio 进程随原会话退出。首次接入可在图谱控制台的集成页面应用配置。已有的自定义启动参数、远程地址和 Codex 的逐工具审批设置会保留。

支持 Streamable HTTP 的其他客户端也可以使用 `http://127.0.0.1:<端口>/mcp`，并设置请求头 `Authorization: Bearer <凭据>`。凭据位于 Electron 用户数据目录的 `desktop-mcp.token`，也可以从本机已迁移的 Codex 配置读取。每个客户端有独立的 MCP 会话，共用桌面进程和项目监听器。索引期间可能短暂出现索引工作进程。

使用这个地址时，桌面服务需要保持运行。如需恢复独立 stdio 模式，可以停止桌面服务，然后运行独立 CLI 的 `install -y` 并重启客户端。

## 测试与检查

```powershell
npm test
npm run check
```

## 构建安装包

先确保对应架构的 UI 二进制存在。Windows 默认从 `../build/c/codebase-memory-mcp.exe` 查找：

```powershell
npm run dist:win
```

macOS 将二进制放入 `desktop/resources/bin/codebase-memory-mcp`，然后在对应架构 macOS 上运行：

```bash
npm run dist:mac:arm64
npm run dist:mac:x64
```

产物输出到 `desktop/dist/`。Release 流程负责二进制暂存、DMG/Setup 打包、挂载校验和 ad-hoc 签名；Apple notarization 尚未接入。
