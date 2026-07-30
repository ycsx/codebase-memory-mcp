const fs = require("node:fs/promises");
const path = require("node:path");
const { app, BrowserWindow, ipcMain } = require("electron");

const renderUserData = process.env.CBM_RENDER_USER_DATA;
if (renderUserData) {
  app.setPath("userData", path.resolve(renderUserData));
}
app.disableHardwareAcceleration();

const status = {
  state: "running",
  managed: true,
  pid: 24680,
  port: 61401,
  binaryPath: "C:\\Program Files\\Codebase Memory\\codebase-memory-mcp.exe",
  startedAt: Date.now() - 372_000,
  uptimeMs: 372_000,
  rssMb: 128.4,
  lastError: null,
};

const logs = [
  { timestamp: Date.now() - 4000, stream: "desktop", message: "正在启动 127.0.0.1:61401" },
  { timestamp: Date.now() - 3000, stream: "stderr", message: "codebase-memory-mcp: console listening on http://127.0.0.1:61401" },
  { timestamp: Date.now() - 2000, stream: "desktop", message: "服务已启动，PID 24680" },
];

for (const channel of ["service:get-status", "service:start", "service:stop", "service:restart", "console:open"]) {
  ipcMain.handle(channel, () => status);
}
ipcMain.handle("service:get-logs", () => logs);
ipcMain.handle("usage:get-stats", () => ({
  available: true,
  retention_days: 30,
  total_calls: 184,
  calls_last_minute: 3,
  calls_last_hour: 29,
  errors: 2,
  active_files: 17,
  unattributed_calls: 31,
  files: [
    { project: "aikb-web", file_path: "src/utils/chat.js", calls: 38, calls_last_hour: 8, primary_tool: "trace_path", last_called_at_ms: Date.now() - 19_000 },
    { project: "aikb-web", file_path: "src/components/ChatPanel.vue", calls: 24, calls_last_hour: 5, primary_tool: "explain_impact", last_called_at_ms: Date.now() - 162_000 },
    { project: "codebase-memory-mcp-source", file_path: "src/mcp/mcp.c", calls: 17, calls_last_hour: 2, primary_tool: "get_code_snippet", last_called_at_ms: Date.now() - 840_000 },
  ],
}));
ipcMain.handle("update:get-status", () => ({
  state: "available",
  currentVersion: "0.9.0",
  latestVersion: "1.0.0",
  progress: null,
  error: null,
  supported: true,
}));
ipcMain.handle("update:check", () => ({ state: "up-to-date", currentVersion: "1.0.0" }));
ipcMain.handle("update:install", () => ({ state: "installing", currentVersion: "0.9.0", latestVersion: "1.0.0" }));
ipcMain.handle("clipboard:write-text", () => true);

app.whenReady().then(async () => {
  const window = new BrowserWindow({
    width: Number(process.env.CBM_RENDER_WIDTH) || 1040,
    height: Number(process.env.CBM_RENDER_HEIGHT) || 900,
    show: false,
    backgroundColor: "#f5f6f2",
    webPreferences: {
      preload: path.join(__dirname, "..", "src", "preload.cjs"),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
    },
  });
  await window.loadFile(path.join(__dirname, "..", "src", "renderer", "index.html"));
  await new Promise((resolve) => setTimeout(resolve, 750));
  const image = await window.webContents.capturePage();
  const output = process.env.CBM_RENDER_OUTPUT || path.join(app.getPath("temp"), "codebase-memory-desktop-smoke.png");
  await fs.writeFile(output, image.toPNG());
  console.log(output);
  app.quit();
});
