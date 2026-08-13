# 安装与构建

本文记录当前 `ycsx/codebase-memory-mcp` 分支的安装、源码构建和发布资产使用方式。GitHub Release 是推荐安装来源；`pkg/` 下的 npm/PyPI/Homebrew 等清单不代表对应注册表一定已经发布最新构建。

## Release 安装

### macOS / Linux

```bash
curl -fsSL https://raw.githubusercontent.com/ycsx/codebase-memory-mcp/main/install.sh | bash
```

需要图谱 UI 时：

```bash
curl -fsSL https://raw.githubusercontent.com/ycsx/codebase-memory-mcp/main/install.sh | bash -s -- --ui
```

脚本会检测系统架构，下载匹配资产并校验 `checksums.txt`。可选参数：`--ui`、`--standard`、`--skip-config`、`--dir=<path>`。Linux 优先使用 `-portable` 资产；macOS 按 Apple Silicon/Intel 选择 arm64/amd64。

### Windows

推荐使用按用户安装的 Setup：

- [Windows AMD64 UI Setup](https://github.com/ycsx/codebase-memory-mcp/releases/latest/download/codebase-memory-mcp-ui-windows-amd64-setup.exe)
- [Windows ARM64 UI Setup](https://github.com/ycsx/codebase-memory-mcp/releases/latest/download/codebase-memory-mcp-ui-windows-arm64-setup.exe)

Setup 不需要管理员权限，会安装 CLI、MCP 二进制和 Desktop 控制器，并保留索引与设置。脚本安装方式：

```powershell
Invoke-WebRequest https://raw.githubusercontent.com/ycsx/codebase-memory-mcp/main/install.ps1 -OutFile install.ps1
Unblock-File .\install.ps1
.\install.ps1
```

脚本执行策略受限时，可在当前 PowerShell 进程使用：

```powershell
Set-ExecutionPolicy -Scope Process Bypass
PowerShell -ExecutionPolicy Bypass -File .\install.ps1
```

### 手动安装

从 [最新 Release](https://github.com/ycsx/codebase-memory-mcp/releases/latest) 下载对应平台的标准包或 UI 包，解压后运行包内安装脚本：

```bash
tar xzf codebase-memory-mcp-*.tar.gz
./install.sh
```

```powershell
Expand-Archive codebase-memory-mcp-windows-amd64.zip -DestinationPath .
Unblock-File .\install.ps1
.\install.ps1
```

安装完成后重启 AI 客户端。可先用 `--dry-run` 或 `--plan` 审核将要修改的文件。

## 从源码构建

### macOS / Linux

依赖：C/C++ 编译器、zlib、Git。macOS 使用 `xcode-select --install`；Debian/Ubuntu 可使用：

```bash
sudo apt install build-essential zlib1g-dev git
```

构建：

```bash
git clone https://github.com/ycsx/codebase-memory-mcp.git
cd codebase-memory-mcp
scripts/build.sh
scripts/build.sh --with-ui
```

二进制位于 `build/c/codebase-memory-mcp`。需要版本号时使用 `scripts/build.sh --version v0.9.0`。

### Windows 原生构建

安装 Git、Node.js 18+ 和 MSYS2。在 MSYS2 CLANG64 中：

```bash
pacman -Syu
pacman -S --needed mingw-w64-clang-x86_64-clang mingw-w64-clang-x86_64-zlib make git
scripts/build.sh --with-ui CC=clang CXX=clang++
```

UI 二进制输出到 `build/c/codebase-memory-mcp.exe`。可在 PowerShell 配置检测到的客户端：

```powershell
.\build\c\codebase-memory-mcp.exe install -y
```

偏好 WSL 时仍可使用：

```powershell
Invoke-WebRequest https://raw.githubusercontent.com/ycsx/codebase-memory-mcp/main/scripts/setup-windows.ps1 -OutFile setup-windows.ps1
.\setup-windows.ps1 -FromSource
```

## 本地 UI

UI 版本把图谱控制台嵌入二进制：

```bash
codebase-memory-mcp console --port=9749 --no-open
```

服务只绑定 `127.0.0.1`。Windows/macOS Desktop 安装包会独立管理该 `console` 子进程；普通 stdio MCP 启动不会自动创建 UI 服务。

## Release 资产清单

发布时应包含：

- `codebase-memory-mcp-<os>-<arch>.tar.gz` 或 `.zip`
- `codebase-memory-mcp-ui-<os>-<arch>.tar.gz` 或 `.zip`
- `codebase-memory-mcp-ui-windows-<arch>-setup.exe`
- `checksums.txt` 以及相应签名、SBOM 和 provenance 资产

注册表包需要单独发布，`pkg/` 中的 manifest 只是发布模板。
