const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const DEFAULT_PORT = 9749;

function binaryName(platform = process.platform) {
  return platform === "win32" ? "codebase-memory-mcp.exe" : "codebase-memory-mcp";
}

function binaryCandidates(options = {}) {
  const env = options.env ?? process.env;
  const platform = options.platform ?? process.platform;
  const appPath = options.appPath ?? path.resolve(__dirname, "..");
  const resourcesPath = options.resourcesPath ?? process.resourcesPath;
  const name = binaryName(platform);
  const candidates = [];

  if (env.CBM_BINARY) {
    candidates.push(path.resolve(env.CBM_BINARY));
  }
  if (resourcesPath) {
    candidates.push(path.resolve(resourcesPath, "..", "..", name));
  }
  if (env.LOCALAPPDATA) {
    candidates.push(
      path.join(env.LOCALAPPDATA, "Programs", "codebase-memory-mcp", name),
    );
  }
  if (resourcesPath) {
    candidates.push(path.join(resourcesPath, "bin", name));
  }
  candidates.push(path.resolve(appPath, "..", "build", "c", name));

  return [...new Set(candidates)];
}

function resolveBinaryPath(options = {}) {
  const existsSync = options.existsSync ?? fs.existsSync;
  return binaryCandidates(options).find((candidate) => existsSync(candidate)) ?? null;
}

function configPath(options = {}) {
  const env = options.env ?? process.env;
  if (env.CBM_CACHE_DIR) {
    return path.join(path.resolve(env.CBM_CACHE_DIR), "config.json");
  }

  const home = options.home ?? env.HOME ?? env.USERPROFILE ?? os.homedir();
  return path.join(home, ".cache", "codebase-memory-mcp", "config.json");
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
