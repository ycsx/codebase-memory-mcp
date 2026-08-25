const { EventEmitter, once } = require("node:events");
const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");
const { execFile: nodeExecFile, spawn: nodeSpawn } = require("node:child_process");
const { readConfiguredPort, resolveBinaryPath } = require("./binary-locator.cjs");

const START_TIMEOUT_MS = 15_000;
const STOP_TIMEOUT_MS = 5_000;
const MAX_LOG_ENTRIES = 600;

function delay(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function validServiceToken(token) {
  return typeof token === "string" && /^[a-f0-9]{48,128}$/i.test(token);
}

function readServiceState(statePath) {
  if (!statePath) {
    return null;
  }
  try {
    const state = JSON.parse(fs.readFileSync(statePath, "utf8"));
    if (!validServiceToken(state?.token) || !Number.isInteger(state?.pid) || state.pid <= 0) {
      return null;
    }
    if (!Number.isInteger(state?.port) || state.port <= 0 || state.port >= 65536) {
      return null;
    }
    return {
      pid: state.pid,
      port: state.port,
      token: state.token,
      startedAt: Number.isFinite(state.startedAt) ? state.startedAt : null,
    };
  } catch {
    return null;
  }
}

function writeServiceState(statePath, state) {
  if (!statePath || !state) {
    return;
  }
  try {
    fs.mkdirSync(path.dirname(statePath), { recursive: true });
    fs.writeFileSync(statePath, `${JSON.stringify(state)}\n`, "utf8");
  } catch {
    // Persistence is best effort; the current Desktop process still owns the child.
  }
}

function removeServiceState(statePath) {
  if (!statePath) {
    return;
  }
  try {
    fs.unlinkSync(statePath);
  } catch (error) {
    if (error?.code !== "ENOENT") {
      return;
    }
  }
}

function execFileText(file, args, options = {}) {
  const execFileImpl = options.execFileImpl ?? nodeExecFile;
  return new Promise((resolve, reject) => {
    execFileImpl(
      file,
      args,
      { encoding: "utf8", timeout: options.timeoutMs ?? 1800, windowsHide: true },
      (error, stdout) => {
        if (error) {
          reject(error);
          return;
        }
        resolve(stdout);
      },
    );
  });
}

function parseProcessIds(output) {
  return new Set(
    String(output)
      .split(/\r?\n/)
      .map((line) => Number(line.trim()))
      .filter((pid) => Number.isInteger(pid) && pid > 0),
  );
}

function parseListeningPorts(output, allowedPids) {
  const ports = [];
  for (const line of String(output).split(/\r?\n/)) {
    const match = line.match(/^\s*TCP\s+(?:127\.0\.0\.1|\[::1\]):(\d+)\s+\S+\s+LISTENING\s+(\d+)\s*$/i);
    if (!match || !allowedPids.has(Number(match[2]))) {
      continue;
    }
    const port = Number(match[1]);
    if (port > 0 && port < 65536 && !ports.includes(port)) {
      ports.push(port);
    }
  }
  return ports;
}

async function discoverWindowsConsolePorts(options = {}) {
  const [pidOutput, netstatOutput] = await Promise.all([
    execFileText(
      "powershell.exe",
      [
        "-NoProfile",
        "-NonInteractive",
        "-Command",
        "Get-Process -Name codebase-memory-mcp -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id",
      ],
      options,
    ),
    execFileText("netstat.exe", ["-ano", "-p", "tcp"], options),
  ]);
  return parseListeningPorts(netstatOutput, parseProcessIds(pidOutput));
}

async function probeConsole(port, options = {}) {
  const fetchImpl = options.fetchImpl ?? globalThis.fetch;
  const timeoutMs = options.timeoutMs ?? 900;
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);

  try {
    const response = await fetchImpl(`http://127.0.0.1:${port}/api/processes`, {
      cache: "no-store",
      redirect: "error",
      signal: controller.signal,
    });
    if (!response.ok) {
      return { reachable: false, pid: null };
    }
    const body = await response.json();
    const pid = Number(body.self_pid);
    return {
      reachable: true,
      pid: Number.isInteger(pid) && pid > 0 ? pid : null,
      rssMb: Number.isFinite(Number(body.self_rss_mb)) ? Number(body.self_rss_mb) : null,
      serviceToken: validServiceToken(body.service_token) ? body.service_token : null,
    };
  } catch {
    return { reachable: false, pid: null };
  } finally {
    clearTimeout(timer);
  }
}

async function fetchUsageStats(port, options = {}) {
  const fetchImpl = options.fetchImpl ?? globalThis.fetch;
  const timeoutMs = options.timeoutMs ?? 1200;
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);

  try {
    const response = await fetchImpl(`http://127.0.0.1:${port}/api/usage-stats?limit=12`, {
      cache: "no-store",
      redirect: "error",
      signal: controller.signal,
    });
    if (!response.ok) {
      return { available: false, files: [] };
    }
    return await response.json();
  } catch {
    return { available: false, files: [] };
  } finally {
    clearTimeout(timer);
  }
}

function classifyHealth(probe, ownedPid) {
  if (!probe?.reachable) {
    return "stopped";
  }
  return ownedPid && probe.pid === ownedPid ? "running" : "external";
}

function isOwnedProcess(child, probe) {
  return Boolean(child && probe?.reachable && probe.pid === child.pid);
}

function isChildAlive(child) {
  return Boolean(child && child.exitCode === null && !child.killed);
}

class ServiceManager extends EventEmitter {
  constructor(options = {}) {
    super();
    this.appPath = options.appPath;
    this.resourcesPath = options.resourcesPath;
    this.env = options.env ?? process.env;
    this.platform = options.platform ?? process.platform;
    this.spawn = options.spawn ?? nodeSpawn;
    this.probe = options.probe ?? probeConsole;
    this.fetchUsage = options.fetchUsage ?? fetchUsageStats;
    this.statePath = options.statePath ?? null;
    this.killProcess = options.killProcess ?? ((pid) => process.kill(pid));
    this.tokenFactory = options.tokenFactory ?? (() => crypto.randomBytes(32).toString("hex"));
    this.discoverPorts = options.discoverPorts ?? (this.platform === "win32"
      ? discoverWindowsConsolePorts
      : async () => []);
    this.resolveBinary = options.resolveBinary ?? (() => resolveBinaryPath({
      appPath: this.appPath,
      resourcesPath: this.resourcesPath,
      env: this.env,
      platform: this.platform,
    }));
    this.readPort = options.readPort ?? (() => readConfiguredPort({
      env: this.env,
      platform: this.platform,
    }));
    this.child = null;
    this.adoptedPid = null;
    this.persistedState = readServiceState(this.statePath);
    this.serviceToken = this.persistedState?.token ?? null;
    this.binaryPath = this.resolveBinary();
    this.port = this.readPort();
    this.state = "stopped";
    this.startedAt = null;
    this.lastError = null;
    this.lastProbe = { reachable: false, pid: null };
    this.logs = [];
    this.operation = null;
    this.discoveryCache = { timestamp: 0, ports: [] };
    this.discoveryPromise = null;
  }

  snapshot() {
    const managed = this.isManagedProcess();
    const pid = this.lastProbe.reachable
      ? this.lastProbe.pid
      : isChildAlive(this.child)
        ? this.child.pid
        : null;
    return {
      state: this.state,
      managed,
      pid,
      port: this.port,
      binaryPath: this.binaryPath,
      startedAt: managed ? this.startedAt : null,
      uptimeMs: managed && this.startedAt ? Date.now() - this.startedAt : null,
      rssMb: this.lastProbe.rssMb ?? null,
      lastError: this.lastError,
    };
  }

  isManagedProcess(probe = this.lastProbe) {
    if (isOwnedProcess(this.child, probe)) {
      return true;
    }
    return Boolean(this.adoptedPid && this.serviceToken && probe?.reachable &&
      probe.pid === this.adoptedPid && probe.serviceToken === this.serviceToken);
  }

  isPersistedService(probe) {
    return Boolean(this.persistedState && this.serviceToken && probe?.reachable &&
      probe.pid === this.persistedState.pid && probe.serviceToken === this.serviceToken);
  }

  saveServiceState(pid) {
    this.persistedState = {
      pid,
      port: this.port,
      token: this.serviceToken,
      startedAt: this.startedAt,
    };
    writeServiceState(this.statePath, this.persistedState);
  }

  clearServiceState() {
    this.persistedState = null;
    this.serviceToken = null;
    this.adoptedPid = null;
    removeServiceState(this.statePath);
  }

  getLogs() {
    return this.logs.slice();
  }

  getUsageStats() {
    return this.fetchUsage(this.port);
  }

  appendLog(stream, message) {
    const clean = String(message).replace(/\r$/, "").trimEnd();
    if (!clean) {
      return;
    }
    this.logs.push({ timestamp: Date.now(), stream, message: clean });
    if (this.logs.length > MAX_LOG_ENTRIES) {
      this.logs.splice(0, this.logs.length - MAX_LOG_ENTRIES);
    }
    this.emit("logs", this.getLogs());
  }

  setState(state, error = null) {
    this.state = state;
    this.lastError = error;
    this.emit("status", this.snapshot());
  }

  async refresh() {
    const configuredPort = this.readPort();
    this.port = configuredPort;
    this.binaryPath = this.resolveBinary();
    this.lastProbe = await this.probe(this.port);

    if (this.persistedState?.port && this.persistedState.port !== configuredPort &&
        !this.isPersistedService(this.lastProbe)) {
      const persistedProbe = await this.probe(this.persistedState.port);
      if (this.isPersistedService(persistedProbe)) {
        this.port = this.persistedState.port;
        this.lastProbe = persistedProbe;
      }
    }

    if (!this.lastProbe.reachable && !isChildAlive(this.child)) {
      const discovered = await this.findExternalConsole(configuredPort);
      if (discovered) {
        this.port = discovered.port;
        this.lastProbe = discovered.probe;
      }
    }

    if (this.lastProbe.reachable) {
      if (isOwnedProcess(this.child, this.lastProbe)) {
        this.adoptedPid = null;
        this.setState("running");
      } else if (this.isPersistedService(this.lastProbe)) {
        this.adoptedPid = this.lastProbe.pid;
        this.startedAt = this.persistedState.startedAt;
        this.setState("running");
      } else {
        this.setState("external");
      }
    } else if (isChildAlive(this.child)) {
      if (this.state !== "stopping") {
        this.setState("starting", this.lastError);
      }
    } else if (this.state !== "error") {
      if (!isChildAlive(this.child)) {
        this.clearServiceState();
      }
      this.setState("stopped");
    }
    return this.snapshot();
  }

  async findExternalConsole(configuredPort) {
    const now = Date.now();
    if (now - this.discoveryCache.timestamp > 8000) {
      if (!this.discoveryPromise) {
        this.discoveryPromise = Promise.resolve()
          .then(() => this.discoverPorts())
          .catch(() => [])
          .then((ports) => {
            this.discoveryCache = {
              timestamp: Date.now(),
              ports: ports.filter((port) => port !== configuredPort),
            };
            return this.discoveryCache.ports;
          })
          .finally(() => {
            this.discoveryPromise = null;
          });
      }
      await this.discoveryPromise;
    }

    for (const port of this.discoveryCache.ports) {
      const probe = await this.probe(port);
      if (probe.reachable) {
        return { port, probe };
      }
    }
    return null;
  }

  runExclusive(action) {
    if (this.operation) {
      return Promise.reject(new Error("另一项服务操作正在进行，请稍后重试"));
    }
    this.operation = Promise.resolve()
      .then(action)
      .finally(() => {
        this.operation = null;
      });
    return this.operation;
  }

  start() {
    return this.runExclusive(() => this.startInternal());
  }

  async startInternal() {
    const current = await this.refresh();
    if (current.state === "running" || current.state === "external") {
      return current;
    }
    if (!this.binaryPath) {
      const message = "未找到 codebase-memory-mcp 可执行文件";
      this.appendLog("desktop", message);
      this.setState("error", message);
      throw new Error(message);
    }

    this.logs = [];
    this.startedAt = Date.now();
    this.adoptedPid = null;
    this.serviceToken = this.tokenFactory();
    this.lastProbe = { reachable: false, pid: null };
    this.setState("starting");
    this.appendLog("desktop", `正在启动 127.0.0.1:${this.port}`);

    let child;
    try {
      child = this.spawn(
        this.binaryPath,
        ["console", "--no-open", `--port=${this.port}`],
        {
          cwd: path.dirname(this.binaryPath),
          env: { ...this.env, CBM_DESKTOP_SERVICE_TOKEN: this.serviceToken },
          detached: true,
          windowsHide: true,
          // Keep stdin detached, but capture the service streams so the Desktop
          // console can display runtime diagnostics after startup.
          stdio: ["ignore", "pipe", "pipe"],
        },
      );
    } catch (error) {
      const message = `启动失败：${error.message}`;
      this.startedAt = null;
      this.setState("error", message);
      this.appendLog("desktop", message);
      throw new Error(message);
    }

    this.child = child;
    this.saveServiceState(child.pid);
    this.attachChild(child);
    child.unref?.();

    const deadline = Date.now() + START_TIMEOUT_MS;
    while (Date.now() < deadline) {
      if (!isChildAlive(child)) {
        const message = `服务提前退出（退出码 ${child.exitCode ?? "未知"}）`;
        this.setState("error", message);
        throw new Error(message);
      }
      this.lastProbe = await this.probe(this.port);
      if (isOwnedProcess(child, this.lastProbe)) {
        this.setState("running");
        this.appendLog("desktop", `服务已启动，PID ${child.pid}`);
        return this.snapshot();
      }
      if (this.lastProbe.reachable && !isOwnedProcess(child, this.lastProbe)) {
        child.kill();
        const message = `端口 ${this.port} 已被其他服务占用`;
        this.setState("external", message);
        this.appendLog("desktop", message);
        return this.snapshot();
      }
      await delay(180);
    }

    if (isChildAlive(child)) {
      child.kill();
    }
    const message = `服务在 ${START_TIMEOUT_MS / 1000} 秒内未就绪`;
    this.setState("error", message);
    this.appendLog("desktop", message);
    throw new Error(message);
  }

  attachChild(child) {
    const attach = (stream, label) => {
      if (!stream) {
        return;
      }
      let pending = "";
      stream.setEncoding("utf8");
      stream.on("data", (chunk) => {
        pending += chunk;
        const lines = pending.split(/\r?\n/);
        pending = lines.pop() ?? "";
        for (const line of lines) {
          this.appendLog(label, line);
        }
      });
      stream.on("end", () => this.appendLog(label, pending));
    };
    attach(child.stdout, "stdout");
    attach(child.stderr, "stderr");

    child.once("error", (error) => {
      if (this.child !== child) {
        return;
      }
      const message = `进程错误：${error.message}`;
      this.appendLog("desktop", message);
      this.setState("error", message);
    });
    child.once("exit", (code, signal) => {
      if (this.child !== child) {
        return;
      }
      this.child = null;
      this.startedAt = null;
      this.clearServiceState();
      this.lastProbe = { reachable: false, pid: null };
      this.appendLog("desktop", `服务已退出（退出码 ${code ?? "无"}，信号 ${signal ?? "无"}）`);
      if (this.state !== "stopping" && this.state !== "external") {
        const unexpected = code !== 0 && code !== null;
        this.setState(unexpected ? "error" : "stopped", unexpected ? `服务退出码 ${code}` : null);
      }
    });
  }

  stop() {
    return this.runExclusive(() => this.stopInternal());
  }

  async stopInternal() {
    await this.refresh();
    const child = this.child;
    const adoptedPid = this.adoptedPid;
    if ((!child || !isOwnedProcess(child, this.lastProbe)) && !adoptedPid) {
      if (this.lastProbe.reachable) {
        throw new Error("当前服务由其他程序启动，桌面端不会终止该进程");
      }
      this.setState("stopped");
      return this.snapshot();
    }

    this.setState("stopping");
    const pid = child?.pid ?? adoptedPid;
    this.appendLog("desktop", `正在停止 PID ${pid}`);
    if (child && isOwnedProcess(child, this.lastProbe)) {
      const exited = once(child, "exit");
      child.kill();
      await Promise.race([exited, delay(STOP_TIMEOUT_MS)]);

      if (isChildAlive(child)) {
        const message = "服务未能在规定时间内停止";
        this.setState("error", message);
        throw new Error(message);
      }
    } else {
      try {
        this.killProcess(pid);
      } catch (error) {
        if (error?.code !== "ESRCH") {
          throw error;
        }
      }
      const deadline = Date.now() + STOP_TIMEOUT_MS;
      while (Date.now() < deadline) {
        this.lastProbe = await this.probe(this.port);
        if (!this.lastProbe.reachable || this.lastProbe.pid !== pid) {
          break;
        }
        await delay(100);
      }
      if (this.lastProbe.reachable && this.lastProbe.pid === pid) {
        const message = "接管的服务未能在规定时间内停止";
        this.setState("error", message);
        throw new Error(message);
      }
    }

    this.child = null;
    this.startedAt = null;
    this.lastProbe = await this.probe(this.port);
    this.clearServiceState();
    this.setState("stopped");
    return this.snapshot();
  }

  restart() {
    return this.runExclusive(async () => {
      await this.refresh();
      if (!this.isManagedProcess()) {
        throw new Error("只能重启由桌面端启动的服务");
      }
      await this.stopInternal();
      return this.startInternal();
    });
  }
}

module.exports = {
  ServiceManager,
  classifyHealth,
  discoverWindowsConsolePorts,
  fetchUsageStats,
  isOwnedProcess,
  parseListeningPorts,
  parseProcessIds,
  probeConsole,
};
