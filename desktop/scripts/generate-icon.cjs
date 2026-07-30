const fs = require("node:fs/promises");
const path = require("node:path");
const { app, BrowserWindow } = require("electron");

app.whenReady().then(async () => {
  const window = new BrowserWindow({
    width: 512,
    height: 512,
    show: false,
    frame: false,
    transparent: true,
    webPreferences: { contextIsolation: true, sandbox: true },
  });
  await window.loadFile(path.join(__dirname, "..", "assets", "icon.svg"));
  const image = await window.webContents.capturePage({ x: 0, y: 0, width: 512, height: 512 });
  const output = path.join(__dirname, "..", "assets", "icon.png");
  await fs.writeFile(output, image.toPNG());
  console.log(output);
  app.quit();
});
