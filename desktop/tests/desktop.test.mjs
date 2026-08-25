import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import { existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { readFile } from "node:fs/promises";
import test from "node:test";
import { createRequire } from "node:module";
import os from "node:os";
import path from "node:path";
import { PassThrough } from "node:stream";

const require = createRequire(import.meta.url);
const {
  DEFAULT_PORT,
  binaryCandidates,
  configPath,
  readConfiguredPort,
  resolveBinaryPath,
} = require("../src/binary-locator.cjs");
const {
  ServiceManager,
  classifyHealth,
  fetchUsageStats,
  isOwnedProcess,
  parseListeningPorts,
  parseProcessIds,
  probeConsole,
} = require("../src/service-manager.cjs");

test("binary locator preserves the documented priority", () => {
  const options = {
    appPath: "C:\\repo\\desktop",
    resourcesPath: "C:\\app\\desktop\\resources",
    env: {
      CBM_BINARY: "C:\\custom\\cbm.exe",
      LOCALAPPDATA: "C:\\Users\\test\\AppData\\Local",
    },
    platform: "win32",
  };
  const candidates = binaryCandidates(options);
  assert.equal(candidates[0], "C:\\custom\\cbm.exe");
  assert.equal(candidates[1], "C:\\app\\codebase-memory-mcp.exe");
  assert.match(candidates[2], /Programs\\codebase-memory-mcp\\codebase-memory-mcp\.exe$/);
  assert.equal(candidates[3], "C:\\app\\desktop\\resources\\bin\\codebase-memory-mcp.exe");
  assert.match(candidates[4], /repo\\build\\c\\codebase-memory-mcp\.exe$/);

  const selected = resolveBinaryPath({
    ...options,
    existsSync: (candidate) => candidate === candidates[1],
  });
  assert.equal(selected, candidates[1]);
});

test("macOS packaged binary is discovered inside app resources", () => {
  const candidates = binaryCandidates({
    appPath: "/Applications/Codebase Memory.app/Contents/Resources/app.asar",
    resourcesPath: "/Applications/Codebase Memory.app/Contents/Resources",
    env: {},
    platform: "darwin",
  });
  assert.ok(candidates.some((candidate) => candidate
    .replaceAll("\\", "/")
    .endsWith("/Contents/Resources/bin/codebase-memory-mcp")));
});

test("configured port validates JSON and valid TCP range", () => {
  const env = { USERPROFILE: "C:\\Users\\test" };
  const options = { env, platform: "win32" };
  assert.equal(configPath(options), "C:\\Users\\test\\.cache\\codebase-memory-mcp\\config.json");
  assert.equal(readConfiguredPort({ ...options, readFileSync: () => '{"ui_port":61401}' }), 61401);
  assert.equal(readConfiguredPort({ ...options, readFileSync: () => '{"ui_port":70000}' }), DEFAULT_PORT);
  assert.equal(readConfiguredPort({ ...options, readFileSync: () => "broken" }), DEFAULT_PORT);
});

test("health classification distinguishes owned and external services", () => {
  assert.equal(classifyHealth({ reachable: false, pid: null }, 123), "stopped");
  assert.equal(classifyHealth({ reachable: true, pid: 123 }, 123), "running");
  assert.equal(classifyHealth({ reachable: true, pid: 456 }, 123), "external");
  assert.equal(isOwnedProcess({ pid: 123 }, { reachable: true, pid: 123 }), true);
  assert.equal(isOwnedProcess({ pid: 123 }, { reachable: true, pid: 456 }), false);
});

test("Windows listener discovery only keeps loopback ports owned by matching processes", () => {
  const pids = parseProcessIds("10404\r\n43808\r\n");
  const netstat = [
    "  TCP    127.0.0.1:61401    0.0.0.0:0    LISTENING    43808",
    "  TCP    0.0.0.0:7000       0.0.0.0:0    LISTENING    43808",
    "  TCP    127.0.0.1:9222     0.0.0.0:0    LISTENING    99999",
    "  TCP    [::1]:9749         [::]:0       LISTENING    10404",
  ].join("\r\n");
  assert.deepEqual(parseListeningPorts(netstat, pids), [61401, 9749]);
});

test("service manager discovers an external console on a non-configured port", async () => {
  const manager = new ServiceManager({
    appPath: "C:\\repo\\desktop",
    resourcesPath: "C:\\app\\resources",
    env: {},
    platform: "win32",
    readPort: () => 9749,
    resolveBinary: () => "C:\\app\\codebase-memory-mcp.exe",
    discoverPorts: async () => [61401],
    probe: async (port) => port === 61401
      ? { reachable: true, pid: 9012, rssMb: 42 }
      : { reachable: false, pid: null },
  });

  const status = await manager.refresh();
  assert.equal(status.state, "external");
  assert.equal(status.port, 61401);
  assert.equal(status.pid, 9012);
  assert.equal(status.managed, false);
});

test("service manager refuses to stop a process it did not start", async () => {
  let spawnCalled = false;
  const manager = new ServiceManager({
    appPath: "C:\\repo\\desktop",
    resourcesPath: "C:\\app\\resources",
    env: {},
    platform: "win32",
    readPort: () => 61401,
    resolveBinary: () => "C:\\app\\codebase-memory-mcp.exe",
    probe: async () => ({ reachable: true, pid: 9012 }),
    discoverPorts: async () => [],
    spawn: () => {
      spawnCalled = true;
      return new EventEmitter();
    },
  });

  await assert.rejects(manager.stop(), /其他程序启动/);
  assert.equal(spawnCalled, false);
  assert.equal(manager.snapshot().state, "external");
  assert.equal(manager.snapshot().managed, false);
});

test("Desktop adopts its persisted console after the app restarts", async () => {
  const directory = mkdtempSync(path.join(os.tmpdir(), "cbm-desktop-service-"));
  const statePath = path.join(directory, "service-state.json");
  const token = "a".repeat(64);
  let alive = true;
  let killedPid = null;
  writeFileSync(statePath, JSON.stringify({
    pid: 9012,
    port: 9749,
    token,
    startedAt: Date.now() - 5000,
  }));

  try {
    const manager = new ServiceManager({
      appPath: "C:\\repo\\desktop",
      resourcesPath: "C:\\app\\resources",
      env: {},
      platform: "win32",
      statePath,
      readPort: () => 9749,
      resolveBinary: () => "C:\\app\\codebase-memory-mcp.exe",
      discoverPorts: async () => [],
      probe: async () => alive
        ? { reachable: true, pid: 9012, serviceToken: token, rssMb: 42 }
        : { reachable: false, pid: null },
      killProcess: (pid) => {
        killedPid = pid;
        alive = false;
      },
    });

    const adopted = await manager.refresh();
    assert.equal(adopted.state, "running");
    assert.equal(adopted.managed, true);
    assert.equal(adopted.pid, 9012);

    const stopped = await manager.stop();
    assert.equal(killedPid, 9012);
    assert.equal(stopped.state, "stopped");
    assert.equal(stopped.managed, false);
    assert.equal(existsSync(statePath), false);
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});

test("Desktop starts a detached console and persists its ownership token", async () => {
  const directory = mkdtempSync(path.join(os.tmpdir(), "cbm-desktop-service-"));
  const statePath = path.join(directory, "service-state.json");
  const child = new EventEmitter();
  child.stdout = new PassThrough();
  child.stderr = new PassThrough();
  child.pid = 9013;
  child.exitCode = 1;
  child.killed = false;
  child.unrefCalled = false;
  child.unref = () => {
    child.unrefCalled = true;
  };
  child.kill = () => {
    child.killed = true;
    child.exitCode = 0;
    child.emit("exit", 0, null);
  };
  let spawnOptions = null;
  let probeToken = null;

  try {
    const manager = new ServiceManager({
      appPath: "C:\\repo\\desktop",
      resourcesPath: "C:\\app\\resources",
      env: { USERPROFILE: "C:\\Users\\test" },
      platform: "win32",
      statePath,
      readPort: () => 9749,
      resolveBinary: () => "C:\\app\\codebase-memory-mcp.exe",
      discoverPorts: async () => [],
      spawn: (_binary, _args, options) => {
        spawnOptions = options;
        probeToken = options.env.CBM_DESKTOP_SERVICE_TOKEN;
        child.exitCode = null;
        return child;
      },
      probe: async () => child.exitCode === null
        ? { reachable: true, pid: child.pid, serviceToken: probeToken }
        : { reachable: false, pid: null },
    });

    const started = await manager.start();
    assert.equal(started.state, "running");
    assert.equal(started.managed, true);
    assert.equal(spawnOptions.detached, true);
    assert.deepEqual(spawnOptions.stdio, ["ignore", "pipe", "pipe"]);
    assert.equal(child.unrefCalled, true);
    assert.match(spawnOptions.env.CBM_DESKTOP_SERVICE_TOKEN, /^[a-f0-9]{64}$/);
    const saved = JSON.parse(readFileSync(statePath, "utf8"));
    assert.equal(saved.pid, child.pid);
    assert.equal(saved.token, spawnOptions.env.CBM_DESKTOP_SERVICE_TOKEN);

    const outputReady = new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        manager.off("logs", onLogs);
        reject(new Error("service output was not forwarded to the Desktop log stream"));
      }, 1000);
      function onLogs(logs) {
        const messages = logs.map((entry) => entry.message);
        if (messages.includes("索引完成") && messages.includes("警告：跳过一个文件")) {
          clearTimeout(timer);
          manager.off("logs", onLogs);
          resolve();
        }
      }
      manager.on("logs", onLogs);
    });
    child.stdout.write("索引进度 50%\n索引完成\n");
    child.stderr.write("警告：跳过一个文件\n");
    await outputReady;
    assert.deepEqual(manager.getLogs().slice(-3).map((entry) => [entry.stream, entry.message]), [
      ["stdout", "索引进度 50%"],
      ["stdout", "索引完成"],
      ["stderr", "警告：跳过一个文件"],
    ]);
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});

test("a new Desktop manager reuses the detached console PID without spawning", async () => {
  const directory = mkdtempSync(path.join(os.tmpdir(), "cbm-desktop-service-"));
  const statePath = path.join(directory, "service-state.json");
  const token = "c".repeat(64);
  const child = new EventEmitter();
  child.pid = 9015;
  child.exitCode = null;
  child.killed = false;
  child.unref = () => {};
  child.kill = () => {};
  let spawnCount = 0;
  let alive = false;

  const commonOptions = {
    appPath: "C:\\repo\\desktop",
    resourcesPath: "C:\\app\\resources",
    env: {},
    platform: "win32",
    statePath,
    readPort: () => 9749,
    resolveBinary: () => "C:\\app\\codebase-memory-mcp.exe",
    discoverPorts: async () => [],
    tokenFactory: () => token,
    probe: async () => alive
      ? { reachable: true, pid: child.pid, serviceToken: token }
      : { reachable: false, pid: null },
  };

  try {
    const firstManager = new ServiceManager({
      ...commonOptions,
      spawn: () => {
        spawnCount += 1;
        alive = true;
        return child;
      },
    });
    const firstStatus = await firstManager.start();
    assert.equal(firstStatus.pid, child.pid);
    assert.equal(spawnCount, 1);

    const reopenedManager = new ServiceManager({
      ...commonOptions,
      spawn: () => {
        spawnCount += 1;
        throw new Error("must not spawn a second console");
      },
      killProcess: () => {
        alive = false;
      },
    });
    const reopenedStatus = await reopenedManager.start();
    assert.equal(reopenedStatus.state, "running");
    assert.equal(reopenedStatus.managed, true);
    assert.equal(reopenedStatus.pid, firstStatus.pid);
    assert.equal(spawnCount, 1);
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});

test("usage stats are read from the local console endpoint", async () => {
  let requestedUrl = "";
  const stats = await fetchUsageStats(61401, {
    fetchImpl: async (url) => {
      requestedUrl = url;
      return {
        ok: true,
        json: async () => ({ available: true, total_calls: 7, files: [] }),
      };
    },
  });
  assert.equal(requestedUrl, "http://127.0.0.1:61401/api/usage-stats?limit=12");
  assert.equal(stats.available, true);
  assert.equal(stats.total_calls, 7);
});

test("console probe reads the Desktop ownership token", async () => {
  const probe = await probeConsole(9749, {
    fetchImpl: async () => ({
      ok: true,
      json: async () => ({ self_pid: 9014, self_rss_mb: 12.5, service_token: "b".repeat(64) }),
    }),
  });
  assert.equal(probe.pid, 9014);
  assert.equal(probe.serviceToken, "b".repeat(64));
});

test("desktop exposes branding, AI routing, usage stats, and verified updates", async () => {
  const [html, appSource, preloadSource, mainSource, updaterSource, iconSource, builderConfig, packageJson] = await Promise.all([
    readFile(new URL("../src/renderer/index.html", import.meta.url), "utf8"),
    readFile(new URL("../src/renderer/app.js", import.meta.url), "utf8"),
    readFile(new URL("../src/preload.cjs", import.meta.url), "utf8"),
    readFile(new URL("../src/main.cjs", import.meta.url), "utf8"),
    readFile(new URL("../src/update-manager.cjs", import.meta.url), "utf8"),
    readFile(new URL("../assets/icon.svg", import.meta.url), "utf8"),
    readFile(new URL("../electron-builder.yml", import.meta.url), "utf8"),
    readFile(new URL("../package.json", import.meta.url), "utf8"),
  ]);

  assert.match(html, /<h1>风暴之眼<\/h1>/);
  assert.match(html, /id="connection-prompt"/);
  assert.match(html, /list_projects/);
  assert.match(html, /search_graph/);
  assert.match(html, /trace_path/);
  assert.match(html, /rg\/grep/);
  const promptSource = html.match(/<pre id="connection-prompt">([\s\S]*?)<\/pre>/)?.[1] ?? "";
  assert.doesNotMatch(promptSource, /GitHub|checksums\.txt|install -y/);
  assert.match(html, /id="usage-file-rows"/);
  assert.match(html, /id="update-notice"/);
  assert.match(html, /id="update-dialog"/);
  assert.match(html, /id="app-version"/);
  assert.match(html, /id="version-check-button"/);
  assert.match(html, /id="version-check-result"/);
  assert.match(appSource, /getUsageStats/);
  assert.match(appSource, /installUpdate/);
  assert.match(appSource, /versionCheck\.addEventListener\("click", checkForUpdates\)/);
  assert.match(appSource, /已是最新版本/);
  assert.match(appSource, /cbmDesktop\.copyText/);
  assert.match(preloadSource, /clipboard:write-text/);
  assert.match(preloadSource, /usage:get-stats/);
  assert.match(preloadSource, /update:install/);
  assert.match(mainSource, /usage:get-stats/);
  assert.match(mainSource, /UpdateManager/);
  assert.match(mainSource, /createElectronUpdaterFetch/);
  assert.match(mainSource, /net\.request/);
  assert.doesNotMatch(mainSource, /net\.fetch/);
  assert.match(
    mainSource,
    /async function requestQuit\(\) \{[\s\S]*?clearInterval\(statusTimer\);\s*app\.quit\(\);\s*\}/,
  );
  assert.match(mainSource, /title: "风暴之眼"/);
  assert.match(updaterSource, /checksums\.txt/);
  assert.match(updaterSource, /SHA-256/);
  assert.match(iconSource, /<title>风暴之眼<\/title>/);
  assert.match(builderConfig, /^mac:/m);
  assert.match(builderConfig, /resources\/bin\/codebase-memory-mcp/);
  assert.match(builderConfig, /THIRD_PARTY_NOTICES\.md/);
  assert.match(builderConfig, /hardenedRuntime: false/);
  assert.match(builderConfig, /CFBundleDisplayName: 风暴之眼/);
  assert.doesNotMatch(builderConfig, /^[ \t]+CFBundleName:/m);
  assert.match(builderConfig, /^dmg:/m);
  assert.match(packageJson, /dist:mac:x64/);
  assert.match(packageJson, /dist:mac:arm64/);
});
