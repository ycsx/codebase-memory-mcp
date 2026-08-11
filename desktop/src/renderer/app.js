const elements = {
  headerStatus: document.querySelector("#header-status"),
  headerStatusText: document.querySelector("#header-status-text"),
  state: document.querySelector("#service-state"),
  pid: document.querySelector("#service-pid"),
  address: document.querySelector("#service-address"),
  uptime: document.querySelector("#service-uptime"),
  owner: document.querySelector("#service-owner"),
  memory: document.querySelector("#service-memory"),
  appVersion: document.querySelector("#app-version"),
  binaryPath: document.querySelector("#binary-path"),
  connectionPrompt: document.querySelector("#connection-prompt"),
  copyPrompt: document.querySelector("#copy-prompt-button"),
  copyPromptLabel: document.querySelector("#copy-prompt-label"),
  externalNotice: document.querySelector("#external-notice"),
  start: document.querySelector("#start-button"),
  stop: document.querySelector("#stop-button"),
  restart: document.querySelector("#restart-button"),
  console: document.querySelector("#console-button"),
  logShell: document.querySelector("#log-shell"),
  logOutput: document.querySelector("#log-output"),
  scroll: document.querySelector("#scroll-button"),
  usageTotal: document.querySelector("#usage-total"),
  usageMinute: document.querySelector("#usage-minute"),
  usageHour: document.querySelector("#usage-hour"),
  usageFiles: document.querySelector("#usage-files"),
  usageMeta: document.querySelector("#usage-meta"),
  usageRows: document.querySelector("#usage-file-rows"),
  updateNotice: document.querySelector("#update-notice"),
  updateTitle: document.querySelector("#update-title"),
  updateDescription: document.querySelector("#update-description"),
  updateAction: document.querySelector("#update-action-button"),
  updateActionLabel: document.querySelector("#update-action-label"),
  updateDialog: document.querySelector("#update-dialog"),
  updateDialogCopy: document.querySelector("#update-dialog-copy"),
  updateCancel: document.querySelector("#update-cancel-button"),
  updateConfirm: document.querySelector("#update-confirm-button"),
  versionCheck: document.querySelector("#version-check-button"),
  versionCheckLabel: document.querySelector("#version-check-label"),
  versionCheckResult: document.querySelector("#version-check-result"),
  toast: document.querySelector("#toast"),
  toastMessage: document.querySelector("#toast-message"),
};

const labels = {
  stopped: "已停止",
  starting: "启动中",
  running: "运行中",
  stopping: "停止中",
  external: "外部运行",
  error: "异常",
};

let busy = false;
let toastTimer = null;
let pollInFlight = false;
let usagePollInFlight = false;
let copyResetTimer = null;
let updateStatus = null;
let updateBusy = false;

function formatDuration(ms) {
  if (!Number.isFinite(ms) || ms < 0) {
    return "--";
  }
  const totalSeconds = Math.floor(ms / 1000);
  const hours = Math.floor(totalSeconds / 3600);
  const minutes = Math.floor((totalSeconds % 3600) / 60);
  const seconds = totalSeconds % 60;
  if (hours > 0) {
    return `${hours} 小时 ${minutes} 分`;
  }
  if (minutes > 0) {
    return `${minutes} 分 ${seconds} 秒`;
  }
  return `${seconds} 秒`;
}

function showError(error) {
  const raw = error?.message ?? String(error);
  elements.toastMessage.textContent = raw.replace(/^Error invoking remote method '[^']+': Error: /, "");
  elements.toast.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => {
    elements.toast.hidden = true;
  }, 6500);
}

function renderStatus(status) {
  const state = status?.state ?? "error";
  const transitional = state === "starting" || state === "stopping";
  const active = state === "running" || state === "external";
  const label = labels[state] ?? "未知";

  elements.headerStatus.dataset.state = state;
  elements.headerStatusText.textContent = label;
  elements.state.textContent = label;
  elements.pid.textContent = status?.pid ?? "--";
  elements.address.textContent = `127.0.0.1:${status?.port ?? "--"}`;
  elements.uptime.textContent = formatDuration(status?.uptimeMs);
  elements.owner.textContent = status?.managed ? "桌面端管理" : state === "external" ? "其他程序管理" : "尚未启动";
  elements.memory.textContent = Number.isFinite(status?.rssMb) ? `${status.rssMb.toFixed(1)} MB` : "--";
  elements.binaryPath.textContent = status?.binaryPath ?? "未找到可执行文件";
  elements.externalNotice.hidden = state !== "external";

  elements.start.disabled = busy || active || transitional;
  elements.stop.disabled = busy || !status?.managed || transitional;
  elements.restart.disabled = busy || !status?.managed || transitional;
  elements.console.disabled = busy || transitional;
}

function renderUpdateStatus(status) {
  updateStatus = status;
  const state = status?.state ?? "unsupported";
  const currentVersion = status?.currentVersion ? `v${status.currentVersion}` : "--";
  const latestVersion = status?.latestVersion ? `v${status.latestVersion}` : null;
  elements.appVersion.textContent = currentVersion;
  elements.updateNotice.dataset.state = status?.error ? "error" : state;

  const updateLocked = ["checking", "downloading", "ready", "installing"].includes(state);
  elements.versionCheck.disabled = updateBusy || updateLocked || state === "unsupported" || status?.supported === false;
  elements.versionCheckLabel.textContent = state === "checking"
    ? "检查中"
    : state === "error" ? "重新检查" : "检查版本";
  elements.versionCheckResult.textContent = state === "checking"
    ? "正在连接更新服务器..."
    : state === "up-to-date" ? "已是最新版本"
      : state === "available" && latestVersion ? `可更新至 ${latestVersion}`
        : state === "error" ? "检查失败"
          : state === "unsupported" ? "当前环境不支持应用内更新" : "";

  if (["unsupported", "idle", "up-to-date"].includes(state)) {
    elements.updateNotice.hidden = true;
    return;
  }

  elements.updateNotice.hidden = false;
  elements.updateAction.disabled = updateBusy || ["checking", "downloading", "ready", "installing"].includes(state);
  if (state === "checking") {
    elements.updateTitle.textContent = "正在检查更新";
    elements.updateDescription.textContent = `当前版本 ${currentVersion}`;
    elements.updateActionLabel.textContent = "检查中";
  } else if (state === "available") {
    elements.updateTitle.textContent = status.error ? "更新未完成" : `发现新版本 ${latestVersion}`;
    elements.updateDescription.textContent = status.error ?? `当前版本 ${currentVersion}，安装包将经过 SHA-256 校验。`;
    elements.updateActionLabel.textContent = status.error ? "重试更新" : "下载并安装";
  } else if (state === "downloading") {
    elements.updateTitle.textContent = `正在下载 ${latestVersion ?? "新版本"}`;
    elements.updateDescription.textContent = Number.isFinite(status.progress)
      ? `已完成 ${status.progress}%` : "正在读取安装包";
    elements.updateActionLabel.textContent = "下载中";
  } else if (state === "ready" || state === "installing") {
    elements.updateTitle.textContent = "安装包已校验";
    elements.updateDescription.textContent = "正在停止服务并启动安装程序。";
    elements.updateActionLabel.textContent = "正在安装";
  } else {
    elements.updateTitle.textContent = "检查更新失败";
    elements.updateDescription.textContent = status?.error ?? "暂时无法连接更新服务器。";
    elements.updateActionLabel.textContent = "重新检查";
    elements.updateAction.disabled = updateBusy;
  }
}

async function checkForUpdates() {
  if (updateBusy) {
    return;
  }
  updateBusy = true;
  renderUpdateStatus({ ...updateStatus, state: "checking", error: null });
  try {
    renderUpdateStatus(await window.cbmDesktop.checkForUpdates());
  } catch (error) {
    showError(error);
  } finally {
    updateBusy = false;
    try {
      renderUpdateStatus(await window.cbmDesktop.getUpdateStatus());
    } catch (error) {
      showError(error);
    }
  }
}

function renderLogs(logs) {
  if (!Array.isArray(logs) || logs.length === 0) {
    elements.logOutput.textContent = "等待服务输出...";
    return;
  }
  elements.logOutput.textContent = logs.map((entry) => {
    const time = new Date(entry.timestamp).toLocaleTimeString("zh-CN", { hour12: false });
    return `[${time}] [${entry.stream}] ${entry.message}`;
  }).join("\n");
}

function formatRelativeTime(timestampMs) {
  if (!Number.isFinite(timestampMs) || timestampMs <= 0) {
    return "--";
  }
  const elapsedSeconds = Math.max(0, Math.floor((Date.now() - timestampMs) / 1000));
  if (elapsedSeconds < 10) {
    return "刚刚";
  }
  if (elapsedSeconds < 60) {
    return `${elapsedSeconds} 秒前`;
  }
  if (elapsedSeconds < 3600) {
    return `${Math.floor(elapsedSeconds / 60)} 分钟前`;
  }
  if (elapsedSeconds < 86400) {
    return `${Math.floor(elapsedSeconds / 3600)} 小时前`;
  }
  return new Date(timestampMs).toLocaleString("zh-CN", { hour12: false });
}

function usageCell(text, className = "") {
  const cell = document.createElement("td");
  cell.textContent = text;
  if (className) {
    cell.className = className;
  }
  return cell;
}

function renderUsageStats(stats) {
  const available = stats?.available === true;
  const formatCount = (value) => available && Number.isFinite(Number(value))
    ? new Intl.NumberFormat("zh-CN").format(Number(value))
    : "--";

  elements.usageTotal.textContent = formatCount(stats?.total_calls);
  elements.usageMinute.textContent = formatCount(stats?.calls_last_minute);
  elements.usageHour.textContent = formatCount(stats?.calls_last_hour);
  elements.usageFiles.textContent = formatCount(stats?.active_files);
  elements.usageMeta.textContent = available
    ? `错误 ${formatCount(stats?.errors)} · 未归因 ${formatCount(stats?.unattributed_calls)} · 保留 ${stats?.retention_days ?? 30} 天`
    : "启动本机服务后显示";

  const files = available && Array.isArray(stats?.files) ? stats.files : [];
  if (files.length === 0) {
    const row = document.createElement("tr");
    row.className = "usage-empty";
    const cell = usageCell(available ? "暂无 MCP 调用记录" : "服务未连接，暂时无法读取调用记录");
    cell.colSpan = 5;
    row.append(cell);
    elements.usageRows.replaceChildren(row);
    return;
  }

  const rows = files.map((item) => {
    const row = document.createElement("tr");
    const fileCell = document.createElement("td");
    const filePath = document.createElement("span");
    filePath.className = "usage-file";
    filePath.textContent = item.file_path || "未命名文件";
    filePath.title = item.file_path || "";
    const project = document.createElement("span");
    project.className = "usage-project";
    project.textContent = item.project || "未指定项目";
    fileCell.append(filePath, project);
    row.append(
      fileCell,
      usageCell(formatCount(item.calls), "usage-count"),
      usageCell(formatCount(item.calls_last_hour), "usage-count"),
      usageCell(item.primary_tool || "--", "usage-tool"),
      usageCell(formatRelativeTime(Number(item.last_called_at_ms))),
    );
    return row;
  });
  elements.usageRows.replaceChildren(...rows);
}

async function refreshUsage() {
  if (usagePollInFlight) {
    return;
  }
  usagePollInFlight = true;
  try {
    renderUsageStats(await window.cbmDesktop.getUsageStats());
  } catch {
    renderUsageStats({ available: false, files: [] });
  } finally {
    usagePollInFlight = false;
  }
}

async function refresh() {
  if (pollInFlight) {
    return;
  }
  pollInFlight = true;
  try {
    const [status, logs, updater] = await Promise.all([
      window.cbmDesktop.getStatus(),
      window.cbmDesktop.getLogs(),
      window.cbmDesktop.getUpdateStatus(),
    ]);
    renderStatus(status);
    renderLogs(logs);
    renderUpdateStatus(updater);
  } catch (error) {
    showError(error);
  } finally {
    pollInFlight = false;
  }
}

async function runAction(action) {
  if (busy) {
    return;
  }
  busy = true;
  try {
    const status = await action();
    renderStatus(status);
  } catch (error) {
    showError(error);
  } finally {
    busy = false;
    await refresh();
  }
}

elements.start.addEventListener("click", () => runAction(() => window.cbmDesktop.start()));
elements.stop.addEventListener("click", () => runAction(() => window.cbmDesktop.stop()));
elements.restart.addEventListener("click", () => runAction(() => window.cbmDesktop.restart()));
elements.console.addEventListener("click", () => runAction(() => window.cbmDesktop.openConsole()));
elements.versionCheck.addEventListener("click", checkForUpdates);
elements.updateAction.addEventListener("click", async () => {
  if (updateBusy) {
    return;
  }
  if (updateStatus?.state === "error") {
    await checkForUpdates();
    return;
  }
  if (updateStatus?.state === "available") {
    const version = updateStatus.latestVersion ? ` v${updateStatus.latestVersion}` : "";
    elements.updateDialogCopy.textContent = `将安装${version}。更新会停止相关 MCP 进程并关闭当前程序；安装完成后会重新打开风暴之眼，现有索引和配置会保留。`;
    elements.updateDialog.hidden = false;
    elements.updateCancel.focus();
  }
});
elements.updateCancel.addEventListener("click", () => {
  elements.updateDialog.hidden = true;
});
elements.updateConfirm.addEventListener("click", async () => {
  if (updateBusy) {
    return;
  }
  updateBusy = true;
  elements.updateDialog.hidden = true;
  elements.updateConfirm.disabled = true;
  try {
    renderUpdateStatus(await window.cbmDesktop.installUpdate());
  } catch (error) {
    showError(error);
    renderUpdateStatus(await window.cbmDesktop.getUpdateStatus());
  } finally {
    updateBusy = false;
    elements.updateConfirm.disabled = false;
  }
});
document.addEventListener("keydown", (event) => {
  if (event.key === "Escape" && !elements.updateDialog.hidden && !updateBusy) {
    elements.updateDialog.hidden = true;
  }
});
elements.copyPrompt.addEventListener("click", async () => {
  try {
    await window.cbmDesktop.copyText(elements.connectionPrompt.textContent.trim());
    elements.copyPrompt.dataset.copied = "true";
    elements.copyPromptLabel.textContent = "已复制";
    clearTimeout(copyResetTimer);
    copyResetTimer = setTimeout(() => {
      delete elements.copyPrompt.dataset.copied;
      elements.copyPromptLabel.textContent = "复制提示词";
    }, 1800);
  } catch (error) {
    showError(error);
  }
});
elements.scroll.addEventListener("click", () => {
  elements.logShell.scrollTop = elements.logShell.scrollHeight;
});

window.lucide?.createIcons();
refresh();
refreshUsage();
setInterval(refresh, 1500);
setInterval(refreshUsage, 5000);
