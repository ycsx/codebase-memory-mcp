# 风暴之眼 Desktop

Windows/macOS 桌面服务控制器。它与 `graph-ui` 独立开发，通过启动现有
`codebase-memory-mcp console` 子进程管理本地可视化服务。

## 功能

- 独立桌面窗口和系统托盘
- 启动、停止、重启 MCP 可视化服务
- 展示状态、PID、端口、运行时间、内存和实时日志
- 在独立 Electron 窗口中打开现有可视化控制台
- 只停止桌面端自己启动的进程；外部服务为只读状态
- Windows 支持校验和验证后的 Setup 自动升级；macOS 暂不支持应用内自动升级

## 本地运行

```powershell
cd desktop
npm install
npm start
```

开发模式按以下顺序查找服务二进制：

1. `CBM_BINARY`
2. Electron `resources/bin`
3. `%LOCALAPPDATA%\Programs\codebase-memory-mcp`（Windows）
4. 仓库的 `build/c/codebase-memory-mcp`（Windows 上带 `.exe`）

端口读取自 `~/.cache/codebase-memory-mcp/config.json` 的 `ui_port`，无有效配置时使用
`9749`。如果配置端口未运行，Windows 桌面端会把本机 `codebase-memory-mcp`
进程与 loopback 监听端口做只读匹配，以发现使用其他端口启动的现有控制台。
健康检查只访问 `http://127.0.0.1:<port>/api/processes`。

## 构建安装包

先确保 `../build/c/codebase-memory-mcp.exe` 存在，然后运行：

```powershell
npm run dist:win
```

macOS 需要先将对应架构的 UI 二进制放到
`desktop/resources/bin/codebase-memory-mcp`，然后在相应架构的 macOS 上运行：

```bash
npm run dist:mac:arm64  # Apple Silicon
npm run dist:mac:x64    # Intel
```

产物输出到 `desktop/dist/`。Release 流水线会自动完成二进制暂存、DMG
打包、挂载校验和 ad-hoc 签名验证；Apple notarization 尚未接入。
