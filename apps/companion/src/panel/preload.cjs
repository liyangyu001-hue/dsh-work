/**
 * 面板 · 预加载
 *
 * 面板是唯一能"输入"的窗口（以后会有对话框），所以它是这一层里权限最大的；
 * 但依然只暴露意图，不暴露任何 Node / Electron 对象。
 */

"use strict";

const { contextBridge, ipcRenderer } = require("electron");

contextBridge.exposeInMainWorld("dshworkPanel", {
  /** 主进程推来的状态：{ state, snapshot } */
  onState(callback) {
    ipcRenderer.on("host:state", (_event, payload) => callback(payload));
    return () => ipcRenderer.removeAllListeners("host:state");
  },

  /** 框选回来的结果：{ file, width, height } */
  onCapture(callback) {
    ipcRenderer.on("capture:result", (_event, result) => callback(result));
    return () => ipcRenderer.removeAllListeners("capture:result");
  },

  /** 识别进度：{ phase: "start" | "done" | "error", ... } */
  onRecognize(callback) {
    ipcRenderer.on("recognize", (_event, message) => callback(message));
    return () => ipcRenderer.removeAllListeners("recognize");
  },

  /** 背景底图（桌面截图，data URL）—— 玻璃的"底" */
  onBackdrop(callback) {
    ipcRenderer.on("panel:backdrop", (_event, url) => callback(url));
    return () => ipcRenderer.removeAllListeners("panel:backdrop");
  },

  /** 把一条意图交给主进程排队发送（渲染层不碰网络） */
  intent(intent) {
    ipcRenderer.send("panel:intent", intent);
  },

  /** 请求进入框选（主进程会先藏面板再截） */
  capture() {
    ipcRenderer.send("panel:capture");
  },

  /** 关掉面板 */
  close() {
    ipcRenderer.send("panel:close");
  },

  /** 我准备好了，把当前状态发我 */
  ready() {
    ipcRenderer.send("panel:ready");
  },
});
