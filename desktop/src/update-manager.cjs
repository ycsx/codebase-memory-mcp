const { createHash } = require("node:crypto");
const { EventEmitter } = require("node:events");
const fs = require("node:fs");
const { mkdir, rename, rm } = require("node:fs/promises");
const https = require("node:https");
const path = require("node:path");
const { pipeline } = require("node:stream/promises");
const { Transform } = require("node:stream");

const RELEASE_API_URL = "https://api.github.com/repos/ycsx/codebase-memory-mcp/releases/latest";
const CHECK_INTERVAL_MS = 24 * 60 * 60 * 1000;
const INITIAL_CHECK_DELAY_MS = 5000;
const REQUEST_TIMEOUT_MS = 20000;
const MAX_RELEASE_BYTES = 2 * 1024 * 1024;
const MAX_CHECKSUM_BYTES = 1024 * 1024;
const MAX_INSTALLER_BYTES = 512 * 1024 * 1024;
const ALLOWED_DOWNLOAD_HOSTS = new Set([
  "github.com",
  "objects.githubusercontent.com",
  "release-assets.githubusercontent.com",
]);

function parseVersion(value) {
  const match = /^v?(\d+)\.(\d+)\.(\d+)(?:\.(\d+))?(?:[-+].*)?$/.exec(String(value ?? "").trim());
  return match ? match.slice(1, 5).map((part) => Number(part ?? 0)) : null;
}

function compareVersions(left, right) {
  const leftParts = parseVersion(left);
  const rightParts = parseVersion(right);
  if (!leftParts || !rightParts) {
    throw new Error("无法识别版本号");
  }
  for (let index = 0; index < leftParts.length; index += 1) {
    if (leftParts[index] !== rightParts[index]) {
      return leftParts[index] > rightParts[index] ? 1 : -1;
    }
  }
  return 0;
}

function installerAssetName(platform, arch) {
  if (platform !== "win32") {
    return null;
  }
  const releaseArch = arch === "x64" ? "amd64" : arch === "arm64" ? "arm64" : null;
  return releaseArch ? `codebase-memory-mcp-ui-windows-${releaseArch}-setup.exe` : null;
}

function parseChecksum(contents, assetName) {
  for (const line of String(contents).split(/\r?\n/)) {
    const match = /^([a-fA-F0-9]{64})\s+\*?(.+?)\s*$/.exec(line);
    if (match && match[2] === assetName) {
      return match[1].toLowerCase();
    }
  }
  return null;
}

function assertAllowedDownloadUrl(value) {
  let parsed;
  try {
    parsed = new URL(value);
  } catch {
    throw new Error("Release 下载地址无效");
  }
  if (parsed.protocol !== "https:" || !ALLOWED_DOWNLOAD_HOSTS.has(parsed.hostname)) {
    throw new Error("Release 下载地址不受信任");
  }
  return parsed;
}

async function fetchBoundedText(url, options = {}) {
  const fetchImpl = options.fetchImpl ?? globalThis.fetch;
  if (typeof fetchImpl !== "function") {
    throw new Error("当前运行环境不支持更新检查");
  }
  const maxBytes = options.maxBytes ?? MAX_RELEASE_BYTES;
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), options.timeoutMs ?? REQUEST_TIMEOUT_MS);
  timer.unref?.();
  try {
    const response = await fetchImpl(url, {
      headers: {
        Accept: "application/vnd.github+json",
        "User-Agent": "codebase-memory-desktop-updater",
      },
      redirect: "follow",
      signal: controller.signal,
    });
    if (!response?.ok) {
      throw new Error(`更新服务器返回 HTTP ${response?.status ?? "未知"}`);
    }
    const declaredLength = Number(response.headers?.get?.("content-length"));
    if (Number.isFinite(declaredLength) && declaredLength > maxBytes) {
      throw new Error("更新信息超过允许大小");
    }
    const body = await response.text();
    if (Buffer.byteLength(body, "utf8") > maxBytes) {
      throw new Error("更新信息超过允许大小");
    }
    return body;
  } finally {
    clearTimeout(timer);
  }
}

function requestDownloadResponse(url, redirects = 0) {
  const parsed = assertAllowedDownloadUrl(url);
  if (redirects > 5) {
    return Promise.reject(new Error("Release 下载重定向次数过多"));
  }
  return new Promise((resolve, reject) => {
    const request = https.get(parsed, {
      headers: { "User-Agent": "codebase-memory-desktop-updater" },
    }, (response) => {
      const status = response.statusCode ?? 0;
      if (status >= 300 && status < 400 && response.headers.location) {
        const nextUrl = new URL(response.headers.location, parsed).toString();
        response.resume();
        resolve(requestDownloadResponse(nextUrl, redirects + 1));
        return;
      }
      if (status !== 200) {
        response.resume();
        reject(new Error(`安装包下载返回 HTTP ${status || "未知"}`));
        return;
      }
      resolve(response);
    });
    request.setTimeout(REQUEST_TIMEOUT_MS, () => request.destroy(new Error("安装包下载超时")));
    request.once("error", reject);
  });
}

async function downloadAndHash(url, destination, options = {}) {
  const response = await requestDownloadResponse(url);
  const declaredLength = Number(response.headers["content-length"]);
  if (Number.isFinite(declaredLength) && declaredLength > MAX_INSTALLER_BYTES) {
    response.destroy();
    throw new Error("安装包超过允许大小");
  }

  await mkdir(path.dirname(destination), { recursive: true });
  await rm(destination, { force: true });
  const hash = createHash("sha256");
  let bytes = 0;
  const meter = new Transform({
    transform(chunk, _encoding, callback) {
      bytes += chunk.length;
      if (bytes > MAX_INSTALLER_BYTES) {
        callback(new Error("安装包超过允许大小"));
        return;
      }
      hash.update(chunk);
      options.onProgress?.({ bytes, total: Number.isFinite(declaredLength) ? declaredLength : null });
      callback(null, chunk);
    },
  });

  try {
    await pipeline(response, meter, fs.createWriteStream(destination, { flags: "wx" }));
  } catch (error) {
    await rm(destination, { force: true });
    throw error;
  }
  return { bytes, sha256: hash.digest("hex") };
}

function launchWindowsInstaller(installerPath, options = {}) {
  const spawnImpl = options.spawnImpl ?? require("node:child_process").spawn;
  const args = [
    "/SILENT",
    "/SUPPRESSMSGBOXES",
    "/NORESTART",
    "/CLOSEAPPLICATIONS",
    "/FORCECLOSEAPPLICATIONS",
    "/LaunchAfterUpdate=1",
  ];
  return new Promise((resolve, reject) => {
    const child = spawnImpl(installerPath, args, {
      detached: true,
      stdio: "ignore",
      windowsHide: true,
    });
    child.once("error", reject);
    child.once("spawn", () => {
      child.unref();
      resolve(child.pid);
    });
  });
}

class UpdateManager extends EventEmitter {
  constructor(options = {}) {
    super();
    this.currentVersion = String(options.currentVersion ?? "0.0.0").replace(/^v/, "");
    this.platform = options.platform ?? process.platform;
    this.arch = options.arch ?? process.arch;
    this.assetName = installerAssetName(this.platform, this.arch);
    this.tempDir = options.tempDir;
    this.enabled = options.enabled ?? true;
    this.fetchText = options.fetchText ?? ((url, fetchOptions) => fetchBoundedText(url, {
      ...fetchOptions,
      fetchImpl: options.fetchImpl,
    }));
    this.download = options.download ?? downloadAndHash;
    this.removeFile = options.removeFile ?? ((target) => rm(target, { force: true }));
    this.renameFile = options.renameFile ?? rename;
    this.prepareInstall = options.prepareInstall ?? (async () => undefined);
    this.recoverInstall = options.recoverInstall ?? (async () => undefined);
    this.launchInstaller = options.launchInstaller ?? launchWindowsInstaller;
    this.state = this.enabled && this.assetName ? "idle" : "unsupported";
    this.latestVersion = null;
    this.progress = null;
    this.error = null;
    this.release = null;
    this.checkPromise = null;
    this.installPromise = null;
    this.initialTimer = null;
    this.intervalTimer = null;
  }

  snapshot() {
    return {
      state: this.state,
      currentVersion: this.currentVersion,
      latestVersion: this.latestVersion,
      progress: this.progress,
      error: this.error,
      supported: Boolean(this.enabled && this.assetName),
    };
  }

  setState(state, updates = {}) {
    this.state = state;
    Object.assign(this, updates);
    const snapshot = this.snapshot();
    this.emit("status", snapshot);
    return snapshot;
  }

  start() {
    if (!this.enabled || !this.assetName || this.initialTimer || this.intervalTimer) {
      return;
    }
    this.initialTimer = setTimeout(() => {
      this.initialTimer = null;
      this.check().catch(() => undefined);
    }, INITIAL_CHECK_DELAY_MS);
    this.initialTimer.unref?.();
    this.intervalTimer = setInterval(() => this.check().catch(() => undefined), CHECK_INTERVAL_MS);
    this.intervalTimer.unref?.();
  }

  stop() {
    clearTimeout(this.initialTimer);
    clearInterval(this.intervalTimer);
    this.initialTimer = null;
    this.intervalTimer = null;
  }

  check() {
    if (!this.enabled || !this.assetName) {
      return Promise.resolve(this.snapshot());
    }
    if (this.checkPromise) {
      return this.checkPromise;
    }
    if (["downloading", "ready", "installing"].includes(this.state)) {
      return Promise.resolve(this.snapshot());
    }
    this.checkPromise = this.checkInternal().finally(() => {
      this.checkPromise = null;
    });
    return this.checkPromise;
  }

  async checkInternal() {
    this.setState("checking", { error: null, progress: null });
    try {
      const raw = await this.fetchText(RELEASE_API_URL, { maxBytes: MAX_RELEASE_BYTES });
      const release = JSON.parse(raw);
      if (!release || release.draft || release.prerelease || !Array.isArray(release.assets)) {
        throw new Error("最新正式 Release 信息无效");
      }
      const latestVersion = String(release.tag_name ?? "").replace(/^v/, "");
      compareVersions(latestVersion, this.currentVersion);
      this.latestVersion = latestVersion;
      if (compareVersions(latestVersion, this.currentVersion) <= 0) {
        this.release = null;
        return this.setState("up-to-date", { error: null, progress: null });
      }

      const installer = release.assets.find((asset) => asset?.name === this.assetName);
      const checksums = release.assets.find((asset) => asset?.name === "checksums.txt");
      if (!installer?.browser_download_url || !checksums?.browser_download_url) {
        throw new Error(`Release 缺少 ${this.assetName} 或 checksums.txt`);
      }
      const assetSize = Number(installer.size);
      if (Number.isFinite(assetSize) && assetSize > MAX_INSTALLER_BYTES) {
        throw new Error("Release 安装包超过允许大小");
      }
      assertAllowedDownloadUrl(installer.browser_download_url);
      assertAllowedDownloadUrl(checksums.browser_download_url);

      this.release = {
        assetSize: assetSize || null,
        assetUrl: installer.browser_download_url,
        checksumUrl: checksums.browser_download_url,
      };
      return this.setState("available", { error: null, progress: null });
    } catch (error) {
      this.release = null;
      return this.setState("error", {
        error: error instanceof Error ? error.message : "检查更新失败",
        progress: null,
      });
    }
  }

  install() {
    if (this.installPromise) {
      return this.installPromise;
    }
    if (!this.release || !this.latestVersion || !this.tempDir) {
      return Promise.reject(new Error("当前没有可安装的更新"));
    }
    this.installPromise = this.installInternal().finally(() => {
      this.installPromise = null;
    });
    return this.installPromise;
  }

  async installInternal() {
    const installerPath = path.join(
      this.tempDir,
      `codebase-memory-mcp-${this.latestVersion}-${this.arch}-setup.exe`,
    );
    const partialPath = `${installerPath}.partial`;
    let prepared = false;
    let prepareContext = null;
    try {
      this.setState("downloading", { error: null, progress: 0 });
      const checksumText = await this.fetchText(this.release.checksumUrl, {
        maxBytes: MAX_CHECKSUM_BYTES,
      });
      const expectedHash = parseChecksum(checksumText, this.assetName);
      if (!expectedHash) {
        throw new Error("checksums.txt 中缺少当前安装包的校验值");
      }

      let lastProgress = -1;
      const result = await this.download(this.release.assetUrl, partialPath, {
        onProgress: ({ bytes, total }) => {
          const denominator = total || this.release.assetSize;
          if (!Number.isFinite(denominator) || denominator <= 0) {
            return;
          }
          const progress = Math.min(99, Math.floor((bytes / denominator) * 100));
          if (progress !== lastProgress) {
            lastProgress = progress;
            this.setState("downloading", { error: null, progress });
          }
        },
      });
      if (result.sha256.toLowerCase() !== expectedHash) {
        throw new Error("安装包 SHA-256 校验失败");
      }
      if (this.release.assetSize && result.bytes !== this.release.assetSize) {
        throw new Error("安装包大小与 Release 元数据不一致");
      }
      await this.removeFile(installerPath);
      await this.renameFile(partialPath, installerPath);
      this.setState("ready", { error: null, progress: 100 });
      prepareContext = await this.prepareInstall();
      prepared = true;
      this.setState("installing", { error: null, progress: 100 });
      await this.launchInstaller(installerPath);
      return this.snapshot();
    } catch (error) {
      await this.removeFile(partialPath);
      if (prepared) {
        await this.recoverInstall(prepareContext).catch(() => undefined);
      }
      this.setState("available", {
        error: error instanceof Error ? error.message : "安装更新失败",
        progress: null,
      });
      throw error;
    }
  }
}

module.exports = {
  CHECK_INTERVAL_MS,
  RELEASE_API_URL,
  UpdateManager,
  compareVersions,
  downloadAndHash,
  installerAssetName,
  launchWindowsInstaller,
  parseChecksum,
};
