const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const DEFAULT_PORT = 9749;

function binaryName(platform = process.platform) {
  return platform === "win32" ? "codebase-memory-mcp.exe" : "codebase-memory-mcp";
}

function pathForPlatform(platform = process.platform) {
  return platform === "win32" ? path.win32 : path.posix;
}

function binaryCandidates(options = {}) {
  const env = options.env ?? process.env;
  const platform = options.platform ?? process.platform;
  const platformPath = pathForPlatform(platform);
  const appPath = options.appPath ?? path.resolve(__dirname, "..");
  const resourcesPath = options.resourcesPath ?? process.resourcesPath;
  const name = binaryName(platform);
  const candidates = [];

  if (env.CBM_BINARY) {
    candidates.push(platformPath.resolve(env.CBM_BINARY));
  }
  if (resourcesPath) {
    candidates.push(platformPath.resolve(resourcesPath, "..", "..", name));
  }
  if (env.LOCALAPPDATA) {
    candidates.push(
      platformPath.join(env.LOCALAPPDATA, "Programs", "codebase-memory-mcp", name),
    );
  }
  if (resourcesPath) {
    candidates.push(platformPath.join(resourcesPath, "bin", name));
  }
  candidates.push(platformPath.resolve(appPath, "..", "build", "c", name));

  return [...new Set(candidates)];
}

function resolveBinaryPath(options = {}) {
  const existsSync = options.existsSync ?? fs.existsSync;
  return binaryCandidates(options).find((candidate) => existsSync(candidate)) ?? null;
}

function configPath(options = {}) {
  const env = options.env ?? process.env;
  const platform = options.platform ?? process.platform;
  const platformPath = pathForPlatform(platform);
  if (env.CBM_CACHE_DIR) {
    return platformPath.join(platformPath.resolve(env.CBM_CACHE_DIR), "config.json");
  }

  const home = options.home ?? env.HOME ?? env.USERPROFILE ?? os.homedir();
  return platformPath.join(home, ".cache", "codebase-memory-mcp", "config.json");
}

function readConfiguredPort(options = {}) {
  const readFileSync = options.readFileSync ?? fs.readFileSync;
  try {
    const parsed = JSON.parse(readFileSync(configPath(options), "utf8"));
    const port = Number(parsed.ui_port);
    if (Number.isInteger(port) && port > 0 && port < 65536) {
      return port;
    }
  } catch {
    // Missing or invalid user configuration falls back to the server default.
  }
  return DEFAULT_PORT;
}

module.exports = {
  DEFAULT_PORT,
  binaryCandidates,
  configPath,
  readConfiguredPort,
  resolveBinaryPath,
};
