const path = require("node:path");
const {
  app,
  BrowserWindow,
  clipboard,
  ipcMain,
  Menu,
  nativeImage,
  session,
  shell,
  Tray,
} = require("electron");
const { ServiceManager } = require("./service-manager.cjs");
const { UpdateManager } = require("./update-manager.cjs");

const gotLock = app.requestSingleInstanceLock();
if (!gotLock) {
  app.quit();
}

let mainWindow = null;
let consoleWindow = null;
let tray = null;
let service = null;
let updateManager = null;
let statusTimer = null;
let isQuitting = false;

function createTrayImage() {
  return nativeImage
    .createFromPath(path.join(app.getAppPath(), "assets", "icon.png"))
    .resize({ width: 16, height: 16 });
}

function lockNavigation(window, allowedOrigin = null) {
  window.webContents.setWindowOpenHandler(({ url }) => {
    if (url.startsWith("https://") || url.startsWith("http://")) {
      shell.openExternal(url);
    }
    return { action: "deny" };
  });
  window.webContents.on("will-navigate", (event, url) => {
    if (!allowedOrigin || !url.startsWith(allowedOrigin)) {
      event.preventDefault();
    }
  });
}

function createMainWindow() {
  mainWindow = new BrowserWindow({
    width: 1040,
    height: 860,
    minWidth: 840,
    minHeight: 700,
    show: false,
    backgroundColor: "#f5f6f2",
    title: "风暴之眼",
    icon: path.join(app.getAppPath(), "assets", "icon.png"),
    autoHideMenuBar: true,
    webPreferences: {
      preload: path.join(__dirname, "preload.cjs"),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
    },
  });
  mainWindow.loadFile(path.join(__dirname, "renderer", "index.html"));
  lockNavigation(mainWindow);
  mainWindow.once("ready-to-show", () => mainWindow?.show());
  mainWindow.on("close", (event) => {
    if (!isQuitting) {
      event.preventDefault();
      mainWindow.hide();
    }
  });
}

function buildTrayMenu(status = service?.snapshot()) {
  const managed = Boolean(status?.managed);
  const active = status?.state === "running" || status?.state === "external";
  return Menu.buildFromTemplate([
    { label: "显示主窗口", click: () => showMainWindow() },
    { label: "打开可视化控制台", click: () => openConsoleWindow() },
    { type: "separator" },
    {
      label: "启动服务",
      enabled: !active && status?.state !== "starting" && status?.state !== "stopping",
      click: () => service.start().catch(() => undefined),
    },
    {
      label: "停止服务",
      enabled: managed,
      click: () => service.stop().catch(() => undefined),
    },
    { type: "separator" },
    { label: "退出程序", click: () => requestQuit() },
  ]);
}

function createTray() {
  tray = new Tray(createTrayImage());
  tray.setToolTip("风暴之眼");
  tray.setContextMenu(buildTrayMenu());
  tray.on("click", () => showMainWindow());
}

function showMainWindow() {
  if (!mainWindow || mainWindow.isDestroyed()) {
    createMainWindow();
  }
  if (mainWindow.isMinimized()) {
    mainWindow.restore();
  }
  mainWindow.show();
  mainWindow.focus();
}

async function openConsoleWindow() {
  let status = await service.refresh();
  if (status.state === "stopped" || status.state === "error") {
    status = await service.start();
  }
  if (status.state !== "running" && status.state !== "external") {
    throw new Error("服务尚未就绪，无法打开可视化控制台");
  }

  const origin = `http://127.0.0.1:${status.port}`;
  if (consoleWindow && !consoleWindow.isDestroyed()) {
    consoleWindow.show();
    consoleWindow.focus();
    return status;
  }

  consoleWindow = new BrowserWindow({
    width: 1440,
    height: 920,
    minWidth: 960,
    minHeight: 680,
    show: false,
    backgroundColor: "#f5f6f2",
    title: "风暴之眼可视化控制台",
    icon: path.join(app.getAppPath(), "assets", "icon.png"),
    autoHideMenuBar: true,
    webPreferences: {
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
    },
  });
  lockNavigation(consoleWindow, origin);
  consoleWindow.loadURL(origin);
  consoleWindow.once("ready-to-show", () => consoleWindow?.show());
  consoleWindow.on("closed", () => {
    consoleWindow = null;
  });
  return status;
}

async function prepareUpdateInstall() {
  const restartService = Boolean(service?.snapshot().managed);
  const reopenConsole = Boolean(consoleWindow && !consoleWindow.isDestroyed());
  if (restartService) {
    await service.stop();
  }
  if (reopenConsole) {
    consoleWindow.close();
  }
  return { reopenConsole, restartService };
}

async function recoverUpdateInstall(context) {
  if (context?.restartService) {
    await service.start();
  }
  if (context?.reopenConsole) {
    await openConsoleWindow();
  }
}

async function installUpdate() {
  const status = await updateManager.install();
  isQuitting = true;
  updateManager.stop();
  clearInterval(statusTimer);
  setTimeout(() => app.quit(), 80);
  return status;
}

function registerIpc() {
  ipcMain.handle("service:get-status", () => service.refresh());
  ipcMain.handle("service:start", () => service.start());
  ipcMain.handle("service:stop", () => service.stop());
  ipcMain.handle("service:restart", () => service.restart());
  ipcMain.handle("service:get-logs", () => service.getLogs());
  ipcMain.handle("usage:get-stats", () => service.getUsageStats());
  ipcMain.handle("console:open", () => openConsoleWindow());
  ipcMain.handle("update:get-status", () => updateManager.snapshot());
  ipcMain.handle("update:check", () => updateManager.check());
  ipcMain.handle("update:install", () => installUpdate());
  ipcMain.handle("clipboard:write-text", (_event, text) => {
    if (typeof text !== "string" || text.length === 0 || text.length > 12000) {
      throw new Error("无效的复制内容");
    }
    clipboard.writeText(text);
    return true;
  });
}

async function requestQuit() {
  if (isQuitting) {
    return;
  }
  isQuitting = true;
  updateManager?.stop();
  clearInterval(statusTimer);
  try {
    if (service?.snapshot().managed) {
      await service.stop();
    }
  } finally {
    app.quit();
  }
}

if (gotLock) {
  app.on("second-instance", () => showMainWindow());
  app.on("before-quit", (event) => {
    if (!isQuitting && service?.snapshot().managed) {
      event.preventDefault();
      requestQuit();
    } else {
      isQuitting = true;
    }
  });

  app.whenReady().then(() => {
    app.setAppUserModelId("io.github.ycsx.codebasememory.desktop");
    session.defaultSession.setPermissionRequestHandler((_webContents, _permission, callback) => {
      callback(false);
    });
    service = new ServiceManager({
      appPath: app.getAppPath(),
      resourcesPath: process.resourcesPath,
    });
    updateManager = new UpdateManager({
      currentVersion: app.getVersion(),
      platform: process.platform,
      arch: process.arch,
      tempDir: path.join(app.getPath("temp"), "codebase-memory-mcp-updates"),
      enabled: app.isPackaged,
      prepareInstall: prepareUpdateInstall,
      recoverInstall: recoverUpdateInstall,
    });
    service.on("status", (status) => tray?.setContextMenu(buildTrayMenu(status)));
    registerIpc();
    createMainWindow();
    createTray();
    service.refresh().catch(() => undefined);
    updateManager.start();
    statusTimer = setInterval(() => service.refresh().catch(() => undefined), 2500);
  });
}

app.on("window-all-closed", () => {
  // The tray owns the application lifecycle.
});
