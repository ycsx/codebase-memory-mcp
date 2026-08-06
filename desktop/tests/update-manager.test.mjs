import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { EventEmitter } from "node:events";
import { mkdtemp, readFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import test from "node:test";
import { createRequire } from "node:module";

const require = createRequire(import.meta.url);
const {
  RELEASE_API_URL,
  UpdateManager,
  compareVersions,
  installerAssetName,
  launchWindowsInstaller,
  parseChecksum,
} = require("../src/update-manager.cjs");

const INSTALLER_NAME = "codebase-memory-mcp-ui-windows-amd64-setup.exe";
const INSTALLER_URL = `https://github.com/ycsx/codebase-memory-mcp/releases/download/v1.2.0/${INSTALLER_NAME}`;
const CHECKSUM_URL = "https://github.com/ycsx/codebase-memory-mcp/releases/download/v1.2.0/checksums.txt";
const REDIRECTED_INSTALLER_URL = "https://release-assets.githubusercontent.com/github-production-release-asset/update.exe";

function releaseJson(overrides = {}) {
  return JSON.stringify({
    tag_name: "v1.2.0",
    draft: false,
    prerelease: false,
    assets: [
      { name: INSTALLER_NAME, browser_download_url: INSTALLER_URL, size: 15 },
      { name: "checksums.txt", browser_download_url: CHECKSUM_URL, size: 96 },
    ],
    ...overrides,
  });
}

test("desktop updater compares release versions and selects Windows architecture assets", () => {
  assert.equal(compareVersions("v1.2.0", "1.1.9"), 1);
  assert.equal(compareVersions("1.2.0", "v1.2.0"), 0);
  assert.equal(compareVersions("1.2.0", "1.2.1"), -1);
  assert.equal(installerAssetName("win32", "x64"), INSTALLER_NAME);
  assert.equal(installerAssetName("win32", "arm64"), "codebase-memory-mcp-ui-windows-arm64-setup.exe");
  assert.equal(installerAssetName("linux", "x64"), null);
});

test("checksum parser requires the exact release asset name", () => {
  const expected = "a".repeat(64);
  const body = `${"b".repeat(64)}  prefix-${INSTALLER_NAME}\n${expected}  ${INSTALLER_NAME}\n`;
  assert.equal(parseChecksum(body, INSTALLER_NAME), expected);
  assert.equal(parseChecksum(body, "missing.exe"), null);
});

test("update manager reports a newer complete GitHub Release", async () => {
  const manager = new UpdateManager({
    currentVersion: "1.1.0",
    platform: "win32",
    arch: "x64",
    tempDir: "C:\\tmp\\unused",
    fetchText: async (url) => {
      assert.equal(url, RELEASE_API_URL);
      return releaseJson();
    },
  });

  const status = await manager.check();
  assert.equal(status.state, "available");
  assert.equal(status.currentVersion, "1.1.0");
  assert.equal(status.latestVersion, "1.2.0");
});

test("update manager rejects a Release without the expected installer", async () => {
  const manager = new UpdateManager({
    currentVersion: "1.1.0",
    platform: "win32",
    arch: "x64",
    tempDir: "C:\\tmp\\unused",
    fetchText: async () => releaseJson({ assets: [] }),
  });

  const status = await manager.check();
  assert.equal(status.state, "error");
  assert.match(status.error, /Release 缺少/);
});

test("an older Release does not require installer assets", async () => {
  const manager = new UpdateManager({
    currentVersion: "1.3.0",
    platform: "win32",
    arch: "x64",
    tempDir: "C:\\tmp\\unused",
    fetchText: async () => releaseJson({ assets: [] }),
  });

  const status = await manager.check();
  assert.equal(status.state, "up-to-date");
  assert.equal(status.latestVersion, "1.2.0");
  assert.equal(status.error, null);
});

test("default updater transport uses the injected Electron-compatible fetch implementation", async () => {
  const payload = Buffer.from("electron-net-update");
  const checksum = createHash("sha256").update(payload).digest("hex");
  const tempDir = await mkdtemp(path.join(tmpdir(), "cbm-updater-"));
  const requestedUrls = [];
  let launchedPath = null;
  const manager = new UpdateManager({
    currentVersion: "1.1.0",
    platform: "win32",
    arch: "x64",
    tempDir,
    fetchImpl: async (url) => {
      requestedUrls.push(url);
      if (url === RELEASE_API_URL) {
        return new Response(releaseJson({
          assets: [
            { name: INSTALLER_NAME, browser_download_url: INSTALLER_URL, size: payload.length },
            { name: "checksums.txt", browser_download_url: CHECKSUM_URL, size: 96 },
          ],
        }));
      }
      if (url === CHECKSUM_URL) {
        return new Response(`${checksum}  ${INSTALLER_NAME}\n`);
      }
      if (url === INSTALLER_URL) {
        return new Response(null, {
          status: 302,
          headers: { location: REDIRECTED_INSTALLER_URL },
        });
      }
      assert.equal(url, REDIRECTED_INSTALLER_URL);
      return new Response(payload, {
        headers: { "content-length": String(payload.length) },
      });
    },
    launchInstaller: async (installerPath) => {
      launchedPath = installerPath;
    },
  });

  try {
    assert.equal((await manager.check()).state, "available");
    assert.equal((await manager.install()).state, "installing");
    assert.deepEqual(requestedUrls, [
      RELEASE_API_URL,
      CHECKSUM_URL,
      INSTALLER_URL,
      REDIRECTED_INSTALLER_URL,
    ]);
    assert.deepEqual(await readFile(launchedPath), payload);
  } finally {
    await rm(tempDir, { recursive: true, force: true });
  }
});

test("fetch failures retain the underlying network error code", async () => {
  const rootCause = Object.assign(new Error("socket reset"), { code: "ECONNRESET" });
  const fetchFailure = Object.assign(new TypeError("fetch failed"), { cause: rootCause });
  const manager = new UpdateManager({
    currentVersion: "1.1.0",
    platform: "win32",
    arch: "x64",
    tempDir: "C:\\tmp\\unused",
    fetchImpl: async () => {
      throw fetchFailure;
    },
  });

  const status = await manager.check();
  assert.equal(status.state, "error");
  assert.match(status.error, /ECONNRESET/);
  assert.match(status.error, /网络或代理/);
});

test("verified update prepares the app before launching the installer", async () => {
  const payload = Buffer.from("verified-update");
  const checksum = createHash("sha256").update(payload).digest("hex");
  const order = [];
  let launchedPath = null;
  const manager = new UpdateManager({
    currentVersion: "1.1.0",
    platform: "win32",
    arch: "x64",
    tempDir: "C:\\update-tests",
    fetchText: async (url) => {
      if (url === RELEASE_API_URL) {
        return releaseJson({
          assets: [
            { name: INSTALLER_NAME, browser_download_url: INSTALLER_URL, size: payload.length },
            { name: "checksums.txt", browser_download_url: CHECKSUM_URL, size: 96 },
          ],
        });
      }
      assert.equal(url, CHECKSUM_URL);
      return `${checksum}  ${INSTALLER_NAME}\n`;
    },
    download: async (_url, _destination, options) => {
      options.onProgress({ bytes: payload.length, total: payload.length });
      return { bytes: payload.length, sha256: checksum };
    },
    removeFile: async () => undefined,
    renameFile: async () => order.push("rename"),
    prepareInstall: async () => order.push("prepare"),
    launchInstaller: async (installerPath) => {
      order.push("launch");
      launchedPath = installerPath;
    },
  });

  await manager.check();
  const result = await manager.install();
  assert.equal(result.state, "installing");
  assert.deepEqual(order, ["rename", "prepare", "launch"]);
  assert.match(launchedPath, /codebase-memory-mcp-1\.2\.0-x64-setup\.exe$/);
});

test("checksum mismatch refuses to prepare or launch the installer", async () => {
  let prepared = false;
  let launched = false;
  const manager = new UpdateManager({
    currentVersion: "1.1.0",
    platform: "win32",
    arch: "x64",
    tempDir: "C:\\update-tests",
    fetchText: async (url) => url === RELEASE_API_URL
      ? releaseJson({
        assets: [
          { name: INSTALLER_NAME, browser_download_url: INSTALLER_URL, size: 1 },
          { name: "checksums.txt", browser_download_url: CHECKSUM_URL, size: 96 },
        ],
      })
      : `${"a".repeat(64)}  ${INSTALLER_NAME}\n`,
    download: async () => ({ bytes: 1, sha256: "b".repeat(64) }),
    removeFile: async () => undefined,
    prepareInstall: async () => { prepared = true; },
    launchInstaller: async () => { launched = true; },
  });

  await manager.check();
  await assert.rejects(manager.install(), /SHA-256/);
  assert.equal(prepared, false);
  assert.equal(launched, false);
  assert.equal(manager.snapshot().state, "available");
});

test("installer launch failure restores the prepared desktop state", async () => {
  const payload = Buffer.from("x");
  const checksum = createHash("sha256").update(payload).digest("hex");
  const order = [];
  const manager = new UpdateManager({
    currentVersion: "1.1.0",
    platform: "win32",
    arch: "x64",
    tempDir: "C:\\update-tests",
    fetchText: async (url) => url === RELEASE_API_URL
      ? releaseJson({
        assets: [
          { name: INSTALLER_NAME, browser_download_url: INSTALLER_URL, size: payload.length },
          { name: "checksums.txt", browser_download_url: CHECKSUM_URL, size: 96 },
        ],
      })
      : `${checksum}  ${INSTALLER_NAME}\n`,
    download: async () => ({ bytes: payload.length, sha256: checksum }),
    removeFile: async () => undefined,
    renameFile: async () => undefined,
    prepareInstall: async () => {
      order.push("prepare");
      return { restartService: true };
    },
    recoverInstall: async (context) => {
      assert.equal(context.restartService, true);
      order.push("recover");
    },
    launchInstaller: async () => {
      order.push("launch");
      throw new Error("spawn failed");
    },
  });

  await manager.check();
  await assert.rejects(manager.install(), /spawn failed/);
  assert.deepEqual(order, ["prepare", "launch", "recover"]);
  assert.equal(manager.snapshot().state, "available");
});

test("Windows installer launcher uses a detached forced-close upgrade", async () => {
  let observed = null;
  const child = new EventEmitter();
  child.pid = 24680;
  child.unref = () => { observed.unref = true; };
  const launched = launchWindowsInstaller("C:\\tmp\\setup.exe", {
    spawnImpl: (file, args, options) => {
      observed = { file, args, options, unref: false };
      queueMicrotask(() => child.emit("spawn"));
      return child;
    },
  });

  assert.equal(await launched, 24680);
  assert.equal(observed.file, "C:\\tmp\\setup.exe");
  assert.ok(observed.args.includes("/FORCECLOSEAPPLICATIONS"));
  assert.ok(observed.args.includes("/LaunchAfterUpdate=1"));
  assert.equal(observed.options.detached, true);
  assert.equal(observed.unref, true);
});
