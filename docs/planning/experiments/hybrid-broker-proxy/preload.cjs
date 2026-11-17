// Stand-in for the generated @bdk/internal bridge: exposes ONE generic invoke; authority is decided natively.
const { contextBridge, ipcRenderer } = require("electron");
contextBridge.exposeInMainWorld("bdkInternal", { invoke: (command, args) => ipcRenderer.invoke("bdk-internal", command, args) });
