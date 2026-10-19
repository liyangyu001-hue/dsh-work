/**
 * 框选覆盖层 · 预加载
 *
 * 只有三件事：收初始化数据、报选框、报取消。渲染层拿不到任何 Node 能力。
 */

"use strict";

const { contextBridge, ipcRenderer } = require("electron");

contextBridge.exposeInMainWorld("dshworkOverlay", {
  onInit(callback) {
    ipcRenderer.on("overlay:init", (_event, payload) => callback(payload));
  },
  /** 告诉主进程"我这边监听器都挂好了，把初始化数据发过来" */
  ready() {
    ipcRenderer.send("overlay:ready");
  },
  select(displayId, rect) {
    ipcRenderer.send("overlay:select", { displayId, rect });
  },
  cancel(reason) {
    ipcRenderer.send("overlay:cancel", reason);
  },
});
