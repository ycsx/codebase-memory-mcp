# 风暴之眼 Desktop

Windows/macOS 桌面服务控制器。它与 `graph-ui` 独立开发，通过启动现有的 `codebase-memory-mcp console` 子进程管理本地可视化服务。

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
