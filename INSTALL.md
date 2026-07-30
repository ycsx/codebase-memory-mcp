# Installing the ycsx fork

This repository is the `ycsx/codebase-memory-mcp` fork. Its installers, update
checks, and source setup scripts use this fork's GitHub repository.

## Current release status

The fork does not currently have a GitHub Release with binary assets. Until a
release is published, install from source. The release installers intentionally
do not fall back to binaries from the upstream repository.

## macOS and Linux

Prerequisites:

- Git
- A C compiler and C++ compiler (Clang or GCC)
- Make and zlib development headers
- Node.js 18+ and npm when building the UI variant

Source install:

```bash
curl -fsSL https://raw.githubusercontent.com/ycsx/codebase-memory-mcp/main/scripts/setup.sh \
  | bash -s -- --from-source
```

Source install with the embedded graph UI:

```bash
curl -fsSL https://raw.githubusercontent.com/ycsx/codebase-memory-mcp/main/scripts/setup.sh \
  | bash -s -- --from-source --ui
```

The binary is installed to `~/.local/bin/codebase-memory-mcp` by default.

## Windows native build

Install Git, Node.js 18+, and MSYS2. In an MSYS2 CLANG64 shell, install the
native build toolchain:

```bash
pacman -Syu
pacman -S --needed mingw-w64-clang-x86_64-clang \
  mingw-w64-clang-x86_64-zlib make git
```

Clone and build:

```bash
git clone https://github.com/ycsx/codebase-memory-mcp.git
cd codebase-memory-mcp
scripts/build.sh --with-ui CC=clang CXX=clang++
```

The UI build is produced at:

```text
build/c/codebase-memory-mcp.exe
```

Configure detected coding agents from PowerShell or the CLANG64 shell:

```powershell
.\build\c\codebase-memory-mcp.exe install -y
```

The legacy WSL setup path remains available when a Linux binary inside WSL is
preferred:

```powershell
Invoke-WebRequest https://raw.githubusercontent.com/ycsx/codebase-memory-mcp/main/scripts/setup-windows.ps1 -OutFile setup-windows.ps1
.\setup-windows.ps1 -FromSource
```

## GitHub Release installation

After a release is published under
<https://github.com/ycsx/codebase-memory-mcp/releases>, it must include
`checksums.txt` and the matching platform archives:

- `codebase-memory-mcp-<os>-<arch>.tar.gz` or `.zip`
- `codebase-memory-mcp-ui-<os>-<arch>.tar.gz` or `.zip`
- `codebase-memory-mcp-ui-windows-<arch>-setup.exe`

Then the release installers can be used directly.

macOS or Linux:

```bash
curl -fsSL https://raw.githubusercontent.com/ycsx/codebase-memory-mcp/main/install.sh | bash
curl -fsSL https://raw.githubusercontent.com/ycsx/codebase-memory-mcp/main/install.sh | bash -s -- --ui
```

Windows PowerShell:

```powershell
Invoke-WebRequest https://raw.githubusercontent.com/ycsx/codebase-memory-mcp/main/install.ps1 -OutFile install.ps1
Unblock-File .\install.ps1
.\install.ps1 --ui
```

Windows users can instead download the matching `setup.exe`. It installs per
user without administrator rights, adds the CLI to the user `PATH`, creates a
Start menu shortcut, and offers an optional desktop shortcut. Uninstall removes
the application and its shortcuts but deliberately retains indexes and settings
under the user's cache directory.

Registry packages such as npm, PyPI, Homebrew, Scoop, Chocolatey, Winget, and
AUR require separate publication to those registries. Their manifests in
`pkg/` are release templates; they are not a substitute for publishing the
fork's GitHub Release assets.

## Standalone UI

The UI is embedded in the `--with-ui` binary. Start the foreground visual
console with:

```powershell
$env:HOME = 'C:\Users\Public\cbm-home'
$env:USERPROFILE = 'C:\Users\Public\cbm-home'
& '.\build\c\codebase-memory-mcp.exe' console --port=9750
```

The command binds only to `127.0.0.1` and opens the browser automatically. Add
`--no-open` in CI or headless environments. Keep the terminal open while the
console is running. Use a different `HOME` and `USERPROFILE` pair when an
isolated index database is required.
