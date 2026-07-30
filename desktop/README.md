# Codebase Memory Desktop

Windows 桌面服务控制器。它与 `graph-ui` 独立开发，通过启动现有
`codebase-memory-mcp console` 子进程管理本地可视化服务。

## 功能

- 独立桌面窗口和系统托盘
- 启动、停止、重启 MCP 可视化服务
- 展示状态、PID、端口、运行时间、内存和实时日志
- 在独立 Electron 窗口中打开现有可视化控制台
- 只停止桌面端自己启动的进程；外部服务为只读状态

## 本地运行

```powershell
cd desktop
npm install
npm start
```

开发模式按以下顺序查找服务二进制：

1. `CBM_BINARY`
2. Electron `resources/bin`
3. `%LOCALAPPDATA%\Programs\codebase-memory-mcp`
4. 仓库的 `build/c/codebase-memory-mcp.exe`

端口读取自 `~/.cache/codebase-memory-mcp/config.json` 的 `ui_port`，无有效配置时使用
`9749`。如果配置端口未运行，Windows 桌面端会把本机 `codebase-memory-mcp`
进程与 loopback 监听端口做只读匹配，以发现使用其他端口启动的现有控制台。
健康检查只访问 `http://127.0.0.1:<port>/api/processes`。

## 构建 Windows 安装包

先确保 `../build/c/codebase-memory-mcp.exe` 存在，然后运行：

```powershell
npm run dist:win
```

产物输出到 `desktop/dist/`。
