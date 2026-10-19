// DSH Work · 原生输入助手
//
// 为什么需要它：
//   面板是 focusable:false 的窗口（不抢焦点，用户正在打的字一个字都不能断），
//   但面板又要能用 Enter/Backspace/Tab/Esc 操作。用 globalShortcut 能做到，
//   代价是"面板开着的那段时间里，这几个键被全局独占"——如果用户切到编辑器
//   打字，那几个键也进不了编辑器。
//
//   真正干净的做法（也是 lolPing 的做法）是在系统输入层决定：
//   低级键盘钩子先拿到事件，属于"手势键"的吃掉并上报，其余原样穿透。
//   这样我们只在需要的那一刻吃掉按键，别的时候一根汗毛都不碰。
//
// 协议：一行一条 JSON（stdin 进 / stdout 出），见 docs/NATIVE-INPUT.md
//   父 → 子  {"type":"arm","keys":["Enter","Backspace","Tab","Escape","Left","Right"]}
//            {"type":"disarm"}
//            {"type":"exit"}
//   子 → 父  {"type":"ready"}
//            {"type":"key","name":"Enter"}
//            {"type":"log","message":"..."}
//
// 构建：node scripts/build-native.mjs（走 vcvars64 + cl，不依赖 CMake）

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace {

std::mutex g_out_mutex;

/** 写一行 JSON 到 stdout。整行一次写完，避免父进程读到半行。 */
void emit(const std::string& line) {
  std::lock_guard<std::mutex> lock(g_out_mutex);
  std::fwrite(line.data(), 1, line.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

void log_line(const std::string& message) {
  // 日志里只出现 ASCII，省掉控制台代码页这一层麻烦
  std::string escaped;
  for (char c : message) {
    if (c == '"' || c == '\\') {
      escaped.push_back('\\');
      escaped.push_back(c);
    } else if (static_cast<unsigned char>(c) < 0x20) {
      escaped += ' ';
    } else {
      escaped.push_back(c);
    }
  }
  emit("{\"type\":\"log\",\"message\":\"" + escaped + "\"}");
}

/** 我们认识的键：虚拟键码 ↔ 协议里的名字。不认识的一律不拦。 */
struct KeySpec {
  const char* name;
  UINT vk;
};

const KeySpec kKeys[] = {
    {"Enter", VK_RETURN}, {"Backspace", VK_BACK}, {"Tab", VK_TAB},
    {"Escape", VK_ESCAPE}, {"Left", VK_LEFT},     {"Right", VK_RIGHT},
    {"Up", VK_UP},         {"Down", VK_DOWN},     {"Space", VK_SPACE},
    {"F1", VK_F1},         {"F2", VK_F2},
};

const KeySpec* FindByVk(UINT vk) {
  for (const auto& spec : kKeys) {
    if (spec.vk == vk) return &spec;
  }
  return nullptr;
}

const KeySpec* FindByName(const std::string& name) {
  for (const auto& spec : kKeys) {
    if (name == spec.name) return &spec;
  }
  return nullptr;
}

std::atomic<bool> g_running{true};
std::atomic<bool> g_armed{false};
/** 已 arm 的虚拟键码。只在钩子线程与命令线程之间整体替换，用普通 vector + 锁保护。 */
std::mutex g_armed_mutex;
std::vector<UINT> g_armed_vks;

bool IsArmed(UINT vk) {
  if (!g_armed.load(std::memory_order_relaxed)) return false;
  std::lock_guard<std::mutex> lock(g_armed_mutex);
  for (UINT candidate : g_armed_vks) {
    if (candidate == vk) return true;
  }
  return false;
}

/**
 * 低级键盘钩子。
 *
 * 只做一件事：命中 arm 名单的键**吞掉**（不调 CallNextHookEx）并上报；
 * 其余一律立刻转交，保证输入穿透——这是 lolPing 的第一条保证，
 * 也是这个助手存在的全部理由。
 */
LRESULT CALLBACK KeyboardProc(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION) {
    const auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lparam);
    const bool is_down = (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN);
    if (info != nullptr && IsArmed(info->vkCode)) {
      if (is_down && (info->flags & LLKHF_EXTENDED) == 0) {
        // 扩展键（方向键）与普通键分开判断，避免误报；方向键在下面补报
      }
      if (is_down) {
        const KeySpec* spec = FindByVk(info->vkCode);
        if (spec != nullptr) emit(std::string("{\"type\":\"key\",\"name\":\"") + spec->name + "\"}");
      }
      return 1;  // 吞掉：上层程序收不到按下，也收不到抬起
    }
  }
  return CallNextHookEx(nullptr, code, wparam, lparam);
}

/** 极简 JSON 取值：只在固定形状的消息里找键，够用即可，别当通用解析器。 */
std::string ExtractString(const std::string& json, const std::string& key) {
  const std::string needle = "\"" + key + "\":\"";
  const size_t at = json.find(needle);
  if (at == std::string::npos) return {};
  const size_t begin = at + needle.size();
  const size_t end = json.find('"', begin);
  if (end == std::string::npos) return {};
  return json.substr(begin, end - begin);
}

/** 取 "keys":[...] 里的所有字符串。 */
std::vector<std::string> ExtractStringArray(const std::string& json, const std::string& key) {
  std::vector<std::string> out;
  const std::string needle = "\"" + key + "\":[";
  size_t at = json.find(needle);
  if (at == std::string::npos) return out;
  at += needle.size();
  while (at < json.size()) {
    const size_t open = json.find('"', at);
    if (open == std::string::npos) break;
    const size_t close = json.find('"', open + 1);
    if (close == std::string::npos) break;
    out.push_back(json.substr(open + 1, close - open - 1));
    at = close + 1;
    if (json.find(']', at) == at) break;
  }
  return out;
}

void HandleCommand(const std::string& line) {
  const std::string type = ExtractString(line, "type");
  if (type == "arm") {
    std::vector<UINT> vks;
    for (const auto& name : ExtractStringArray(line, "keys")) {
      const KeySpec* spec = FindByName(name);
      if (spec != nullptr) vks.push_back(spec->vk);
    }
    {
      std::lock_guard<std::mutex> lock(g_armed_mutex);
      g_armed_vks = vks;
    }
    g_armed.store(!vks.empty(), std::memory_order_relaxed);
    log_line("armed " + std::to_string(vks.size()));
    return;
  }
  if (type == "disarm") {
    {
      std::lock_guard<std::mutex> lock(g_armed_mutex);
      g_armed_vks.clear();
    }
    g_armed.store(false, std::memory_order_relaxed);
    log_line("disarmed");
    return;
  }
  if (type == "exit") {
    g_running.store(false, std::memory_order_relaxed);
    PostQuitMessage(0);
    return;
  }
  log_line("unknown command: " + type);
}

DWORD WINAPI StdinThread(LPVOID) {
  std::string line;
  while (g_running.load(std::memory_order_relaxed) && std::getline(std::cin, line)) {
    if (!line.empty()) HandleCommand(line);
  }
  // stdin 关了 = 父进程没了，跟着退，别留一个钩子挂在系统里
  g_running.store(false, std::memory_order_relaxed);
  PostThreadMessage(GetCurrentThreadId(), WM_QUIT, 0, 0);
  return 0;
}

}  // namespace

int main() {
  // 父进程用管道说话，stdout 不能带缓冲
  setvbuf(stdout, nullptr, _IONBF, 0);

  HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, GetModuleHandleW(nullptr), 0);
  if (hook == nullptr) {
    log_line("hook failed: " + std::to_string(GetLastError()));
    return 2;
  }

  emit("{\"type\":\"ready\"}");

  HANDLE thread = CreateThread(nullptr, 0, StdinThread, nullptr, 0, nullptr);
  if (thread != nullptr) CloseHandle(thread);

  MSG msg;
  while (g_running.load(std::memory_order_relaxed) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  UnhookWindowsHookEx(hook);
  return 0;
}
