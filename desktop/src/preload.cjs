const { contextBridge, ipcRenderer } = require("electron");

contextBridge.exposeInMainWorld("cbmDesktop", {
  getStatus: () => ipcRenderer.invoke("service:get-status"),
  start: () => ipcRenderer.invoke("service:start"),
  stop: () => ipcRenderer.invoke("service:stop"),
  restart: () => ipcRenderer.invoke("service:restart"),
  getLogs: () => ipcRenderer.invoke("service:get-logs"),
  getUsageStats: () => ipcRenderer.invoke("usage:get-stats"),
  openConsole: () => ipcRenderer.invoke("console:open"),
  getUpdateStatus: () => ipcRenderer.invoke("update:get-status"),
  checkForUpdates: () => ipcRenderer.invoke("update:check"),
  installUpdate: () => ipcRenderer.invoke("update:install"),
  copyText: (text) => ipcRenderer.invoke("clipboard:write-text", text),
});
