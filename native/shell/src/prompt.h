// 一句话输入：不想截图的时候，直接说「我要做 X」，后面交给模型拆。
//
// 为什么用普通窗口 + EDIT 子控件，而不是像小条/面板那样自绘分层窗口：
//   - 分层窗口（UpdateLayeredWindow）上没法放标准 EDIT —— 子控件要往窗口 DC 上画，
//     而分层窗口根本没有可用的窗口 DC；
//   - 自绘一个文本编辑器（光标、选中、输入法候选窗、复制粘贴）是另一个工程量级的事，
//     而且中文输入法几乎必然出问题。这个框一天用几次，值不上。
// 所以：外形朴素一点（直角、深色），但输入法、粘贴、Tab 补全全是系统原生行为。
//
// 职责边界：这里只管「收集一行文字并回一个消息」，不认识 HTTP、不认识任务树。

#pragma once

#include <windows.h>

#include <string>

namespace dshwork::ui::prompt {

constexpr int kPromptWidthDip = 620;
constexpr int kPromptHeightDip = 86;
constexpr UINT kMsgPromptSubmit = WM_APP + 7;  // 回车提交
constexpr UINT kMsgPromptCancel = WM_APP + 8;  // Esc / 失焦

inline wchar_t g_class[64] = L"DSHWorkPrompt";
inline HWND g_window = nullptr;
inline HWND g_edit = nullptr;
inline HFONT g_font = nullptr;
inline HWND g_owner = nullptr;
/** EDIT 会自己吃掉回车，所以单独子类化它，把 Enter/Esc 翻译成我们自己的消息。 */
inline WNDPROC g_edit_proc = nullptr;

inline LRESULT CALLBACK EditProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_KEYDOWN) {
    if (wparam == VK_RETURN) {
      PostMessageW(g_owner, kMsgPromptSubmit, 0, 0);
      return 0;
    }
    if (wparam == VK_ESCAPE) {
      PostMessageW(g_owner, kMsgPromptCancel, 0, 0);
      return 0;
    }
  }
  return CallWindowProcW(g_edit_proc, window, message, wparam, lparam);
}

inline std::wstring g_hint = L"说一件事，我来拆成任务…";

inline void ApplyFont() {
  if (g_font != nullptr) return;
  NONCLIENTMETRICSW metrics = {};
  metrics.cbSize = sizeof(metrics);
  if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
    LOGFONTW font = metrics.lfMessageFont;
    font.lfHeight = -static_cast<LONG>(16 * static_cast<float>(GetDpiForWindow(GetDesktopWindow())) / 96.0f);
    wcscpy_s(font.lfFaceName, L"Microsoft YaHei UI");
    g_font = CreateFontIndirectW(&font);
  }
}

inline void CenterOnCursorMonitor(int dip_w, int dip_h) {
  POINT cursor = {};
  GetCursorPos(&cursor);
  HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
  MONITORINFO info = {};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(monitor, &info)) return;
  const UINT dpi = GetDpiForWindow(g_window);
  const int width = static_cast<int>(dip_w * static_cast<float>(dpi) / 96.0f);
  const int height = static_cast<int>(dip_h * static_cast<float>(dpi) / 96.0f);
  const int x = info.rcWork.left + (info.rcWork.right - info.rcWork.left - width) / 2;
  const int y = info.rcWork.top + (info.rcWork.bottom - info.rcWork.top) / 5;
  SetWindowPos(g_window, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

inline LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_CREATE: {
      ApplyFont();
      g_edit = CreateWindowExW(0, L"EDIT", L"",
                               WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT, 0, 0, 0, 0,
                               window, reinterpret_cast<HMENU>(1), nullptr, nullptr);
      if (g_edit != nullptr && g_font != nullptr) {
        SendMessageW(g_edit, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
      }
      if (g_edit != nullptr) {
        g_edit_proc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(g_edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditProc)));
      }
      return 0;
    }
    case WM_SIZE: {
      if (g_edit != nullptr) {
        const UINT dpi = GetDpiForWindow(window);
        const int pad = static_cast<int>(18 * dpi / 96.0f);
        const int w = LOWORD(lparam);
        const int h = HIWORD(lparam);
        SetWindowPos(g_edit, nullptr, pad, pad, w - pad * 2, h - pad * 2, SWP_NOZORDER);
      }
      return 0;
    }
    case WM_CTLCOLOREDIT: {
      // 深色框里也得是深色输入区，否则白底刺眼
      HDC dc = reinterpret_cast<HDC>(wparam);
      SetBkColor(dc, RGB(24, 26, 32));
      SetTextColor(dc, RGB(236, 238, 244));
      static HBRUSH brush = CreateSolidBrush(RGB(24, 26, 32));
      return reinterpret_cast<LRESULT>(brush);
    }
    case WM_ERASEBKGND: {
      HDC dc = reinterpret_cast<HDC>(wparam);
      RECT rect = {};
      GetClientRect(window, &rect);
      HBRUSH brush = CreateSolidBrush(RGB(20, 22, 28));
      FillRect(dc, &rect, brush);
      DeleteObject(brush);
      // 一圈发丝边，让它有个边界感
      HPEN pen = CreatePen(PS_SOLID, 1, RGB(58, 62, 74));
      HGDIOBJ old_pen = SelectObject(dc, pen);
      HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
      Rectangle(dc, 0, 0, rect.right, rect.bottom);
      SelectObject(dc, old_brush);
      SelectObject(dc, old_pen);
      DeleteObject(pen);
      return 1;
    }
    case WM_KEYDOWN: {
      if (wparam == VK_RETURN) {
        PostMessageW(g_owner, kMsgPromptSubmit, 0, 0);
        return 0;
      }
      if (wparam == VK_ESCAPE) {
        PostMessageW(g_owner, kMsgPromptCancel, 0, 0);
        return 0;
      }
      break;
    }
    // 刻意不做 WM_KILLFOCUS 自动关闭：这个窗口是 WS_EX_TOPMOST 的工具窗，
    // 打开瞬间常常抢不到前台，一失焦就自我了断的体验是「闪一下就没了」。
    default:
      break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

inline void EnsureClass() {
  static bool registered = false;
  if (registered) return;
  WNDCLASSEXW cls = {};
  cls.cbSize = sizeof(cls);
  cls.lpfnWndProc = Proc;
  cls.hInstance = GetModuleHandleW(nullptr);
  cls.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
  cls.lpszClassName = g_class;
  RegisterClassExW(&cls);
  registered = true;
}

inline bool IsOpen() { return g_window != nullptr && IsWindowVisible(g_window) != FALSE; }

inline void Close() {
  if (g_window != nullptr) {
    DestroyWindow(g_window);
    g_window = nullptr;
    g_edit = nullptr;
  }
}

/** 打开输入框。open 时清空上一次的文字，并把焦点给 EDIT。 */
inline HWND Open(HWND owner) {
  g_owner = owner;
  EnsureClass();
  if (g_window == nullptr) {
    g_window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, g_class, L"DSH Work",
                               WS_POPUP, 0, 0, 10, 10, nullptr, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    if (g_window == nullptr) return nullptr;
  }
  if (g_edit != nullptr) SetWindowTextW(g_edit, L"");
  CenterOnCursorMonitor(kPromptWidthDip, kPromptHeightDip);
  ShowWindow(g_window, SW_SHOW);
  // 抢前台：进程不在前台时 SetForegroundWindow 会被系统忽略。
  // 敲一下 Alt 是拿到前台权限的老办法（不产生任何输入效果）。
  if (GetForegroundWindow() != g_window) {
    keybd_event(VK_MENU, 0, 0, 0);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    SetForegroundWindow(g_window);
    BringWindowToTop(g_window);
  }
  if (g_edit != nullptr) {
    SetFocus(g_edit);
    // 让光标先落到 EDIT 里再回来，否则第一次点击会在窗口而不是输入区
    PostMessageW(g_edit, EM_SETSEL, 0, 0);
  }
  return g_window;
}

/** 取走当前文字（UTF-16）。 */
inline std::wstring Text() {
  if (g_edit == nullptr) return L"";
  const int length = GetWindowTextLengthW(g_edit);
  if (length <= 0) return L"";
  std::wstring buffer(static_cast<size_t>(length) + 1, L'\0');
  const int copied = GetWindowTextW(g_edit, buffer.data(), length + 1);
  buffer.resize(static_cast<size_t>(copied < 0 ? 0 : copied));
  return buffer;
}

}  // namespace dshwork::ui::prompt
