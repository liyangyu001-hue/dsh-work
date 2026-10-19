/**
 * 预加载脚本（CommonJS：Electron 的 preload 在 contextIsolation 下必须是 CJS）。
 *
 * 只暴露两个最小能力：
 *   - onHostState(cb)：接收主进程转发的状态
 *   - setPointerOver(bool)：告诉主进程"鼠标进没进小条"，用于切换点击穿透
 * 任何 Node / Electron 对象都不外泄到页面。
 */

const { contextBridge, ipcRenderer } = require("electron");

contextBridge.exposeInMainWorld("dshwork", {
  onHostState(callback) {
    const handler = (_event, payload) => callback(payload);
    ipcRenderer.on("host:state", handler);
    return () => ipcRenderer.removeListener("host:state", handler);
  },
  onHotkey(callback) {
    const handler = (_event, combo) => callback(combo);
    ipcRenderer.on("hotkey", handler);
    return () => ipcRenderer.removeListener("hotkey", handler);
  },
  setPointerOver(over) {
    ipcRenderer.send("bar:pointer", Boolean(over));
  },
  ready() {
    ipcRenderer.send("bar:ready");
  },
});
