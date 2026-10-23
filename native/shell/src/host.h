// 宿主连接（header-only）
//
// 原生层不嵌任何网络栈，就用系统自带的 WinHTTP：零依赖、跟着系统升级、
// 公司代理/证书这些坑系统自己处理。这里只做两件事：
//   1. 读 ~/.dsh/dsh-work/endpoint.json 拿到 url 与 token
//   2. 用 token 拉一次 /dsh-work/api/state
//
// 为什么是轮询不是 SSE：小条只需要"当前节点 + 进度"这几百字节，1 秒一次的
// 代价可以忽略；而流式解析要自己处理分块、重连、半行，收益和风险不成比例。
// 需要更低延迟时再升级，接口不变（见 docs/NATIVE-SHELL.md 的取舍记录）。

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "json.h"

namespace dshwork {

struct Endpoint {
  bool ok = false;
  std::string url;    // 形如 http://127.0.0.1:64001
  std::string token;
  std::string error;  // ok=false 时给人看的原因
};

inline std::wstring Widen(const std::string& text) {
  if (text.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
  return out;
}

inline std::string Narrow(const std::wstring& text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
  return out;
}

/** endpoint.json 的路径。和 Node 側同一口径：优先 DSH_HOME，其次 USERPROFILE\.dsh */
inline std::wstring EndpointPath() {
  wchar_t buffer[MAX_PATH * 2] = {};
  const DWORD env_len = GetEnvironmentVariableW(L"DSH_HOME", buffer, MAX_PATH);
  std::wstring home;
  if (env_len > 0 && env_len < MAX_PATH) {
    home = buffer;
  } else {
    wchar_t profile[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH) == 0) return {};
    home = std::wstring(profile) + L"\\.dsh";
  }
  return home + L"\\dsh-work\\endpoint.json";
}

inline Endpoint ReadEndpoint() {
  Endpoint endpoint;
  const std::wstring path = EndpointPath();
  if (path.empty()) {
    endpoint.error = "找不到用户目录";
    return endpoint;
  }

  std::ifstream file(path);
  if (!file) {
    endpoint.error = "DSH 未启动（读不到 endpoint.json）";
    return endpoint;
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  const std::string raw = buffer.str();

  JValue value;
  if (!ParseJson(raw, value) || !value.IsObject()) {
    endpoint.error = "endpoint.json 解析失败";
    return endpoint;
  }

  const JValue* version = value.Get("version");
  if (version == nullptr || version->type != JValue::Type::Number || version->number != 1) {
    endpoint.error = "endpoint.json 版本不认识";
    return endpoint;
  }

  endpoint.url = value.GetString("url");
  endpoint.token = value.GetString("token");
  if (endpoint.url.empty() || endpoint.token.empty()) {
    endpoint.error = "endpoint.json 缺 url 或 token";
    return endpoint;
  }
  endpoint.ok = true;
  return endpoint;
}

/** 拉一次 JSON。失败时 reason 给人看，避免静默。 */
inline bool HttpGetJson(const Endpoint& endpoint, const std::wstring& path, std::string& body,
                        std::string& reason) {
  URL_COMPONENTS parts = {};
  parts.dwStructSize = sizeof(parts);
  parts.dwHostNameLength = static_cast<DWORD>(-1);
  parts.dwUrlPathLength = static_cast<DWORD>(-1);
  parts.dwSchemeLength = static_cast<DWORD>(-1);

  const std::wstring full = Widen(endpoint.url) + path;
  if (!WinHttpCrackUrl(full.c_str(), 0, 0, &parts)) {
    reason = "url 解析失败";
    return false;
  }

  const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
  const std::wstring request_path(parts.lpszUrlPath, parts.dwUrlPathLength);

  HINTERNET session = WinHttpOpen(L"dsh-work-native/0.1", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (session == nullptr) {
    reason = "WinHttpOpen 失败";
    return false;
  }
  WinHttpSetTimeouts(session, 2000, 2000, 2000, 2000);

  bool ok = false;
  HINTERNET connection = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
  if (connection != nullptr) {
    HINTERNET request = WinHttpOpenRequest(connection, L"GET", request_path.c_str(), nullptr,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (request != nullptr) {
      const std::wstring token_header = L"x-dsh-work-token: " + Widen(endpoint.token);
      WinHttpAddRequestHeaders(request, token_header.c_str(), static_cast<DWORD>(-1),
                               WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
      if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
          WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0;
        DWORD size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
          body.clear();
          DWORD available = 0;
          do {
            if (!WinHttpQueryDataAvailable(request, &available)) break;
            if (available == 0) break;
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read)) break;
            chunk.resize(read);
            body += chunk;
          } while (available > 0);
          ok = !body.empty();
          if (!ok) reason = "响应为空";
        } else if (status == 401 || status == 403) {
          reason = "token 被拒（宿主重启后要重新读 endpoint.json）";
        } else {
          reason = "HTTP " + std::to_string(status);
        }
      } else {
        reason = "请求失败";
      }
      WinHttpCloseHandle(request);
    }
    WinHttpCloseHandle(connection);
  } else {
    reason = "连接失败（DSH 没在跑？）";
  }
  WinHttpCloseHandle(session);
  return ok;
}

/**
 * POST 一段原始字节到宿主。
 *
 * 为什么不复用 HttpPostJson：那个函数的 Content-Type 写死成 application/json，
 * 而截图识别要发的是 image/png 的二进制。两者除了这两处完全一样，所以抽出一层
 * HttpPostRaw，让调用方各自给类型与数据——重复的 WinHTTP 细节只留一份。
 */
inline bool HttpPostRaw(const Endpoint& endpoint, const std::wstring& path, const void* data, size_t size,
                        const std::wstring& content_type, std::string& body, std::string& reason,
                        int receive_ms = 30000, int* out_status = nullptr) {
  if (out_status != nullptr) *out_status = 0;
  URL_COMPONENTS parts = {};
  parts.dwStructSize = sizeof(parts);
  parts.dwHostNameLength = static_cast<DWORD>(-1);
  parts.dwUrlPathLength = static_cast<DWORD>(-1);
  parts.dwSchemeLength = static_cast<DWORD>(-1);

  const std::wstring full = Widen(endpoint.url) + path;
  if (!WinHttpCrackUrl(full.c_str(), 0, 0, &parts)) {
    reason = "url 解析失败";
    return false;
  }
  const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
  const std::wstring request_path(parts.lpszUrlPath, parts.dwUrlPathLength);

  HINTERNET session = WinHttpOpen(L"dsh-work-native/0.1", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (session == nullptr) {
    reason = "WinHttpOpen 失败";
    return false;
  }
  // 识别要等模型，给的比轮询宽：连接 3s、发送 10s、接收按调用方给的来。
  // 讲解（/api/learn）也走这条：模型讲一段要几十秒，30s 会掐在最后几秒。
  WinHttpSetTimeouts(session, 3000, 3000, 10000, receive_ms);

  bool ok = false;
  HINTERNET connection = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
  if (connection != nullptr) {
    HINTERNET request = WinHttpOpenRequest(connection, L"POST", request_path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (request != nullptr) {
      const std::wstring headers = L"x-dsh-work-token: " + Widen(endpoint.token) +
                                   L"\r\nContent-Type: " + content_type + L"\r\n";
      WinHttpAddRequestHeaders(request, headers.c_str(), static_cast<DWORD>(-1),
                               WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
      if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                             size == 0 ? WINHTTP_NO_REQUEST_DATA : const_cast<void*>(data),
                             static_cast<DWORD>(size), static_cast<DWORD>(size), 0) &&
          WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0;
        DWORD size_in = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size_in, WINHTTP_NO_HEADER_INDEX);
        body.clear();
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
          std::string chunk(available, '\0');
          DWORD read = 0;
          if (!WinHttpReadData(request, chunk.data(), available, &read)) break;
          chunk.resize(read);
          body += chunk;
        }
        ok = status >= 200 && status < 300;
        if (out_status != nullptr) *out_status = static_cast<int>(status);
        if (!ok) reason = "HTTP " + std::to_string(status) + " " + body;
      } else {
        reason = "请求失败";
      }
      WinHttpCloseHandle(request);
    }
    WinHttpCloseHandle(connection);
  } else {
    reason = "连接失败（DSH 没在跑？）";
  }
  WinHttpCloseHandle(session);
  return ok;
}

/**
 * 提交一条意图（POST /api/intent）。这是原生层唯一会"改状态"的动作，
 * 而且它不碰本地状态——状态永远以宿主下一次返回的文档为准。
 *
 * receive_ms / out_status 是给「等模型」的那几条路（讲解）加的：HTTP 错误码
 * 也是信息（503 表示宿主没接模型），不能只留一句拼好的 reason 去猜。
 */
inline bool HttpPostJson(const Endpoint& endpoint, const std::wstring& path, const std::string& payload,
                         std::string& body, std::string& reason, int receive_ms = 30000,
                         int* out_status = nullptr) {
  return HttpPostRaw(endpoint, path, payload.data(), payload.size(), L"application/json", body, reason, receive_ms,
                     out_status);
}

/** 提交一张 PNG（POST /api/recognize）。宿主收下就返回 202 + jobId。 */
inline bool HttpPostPng(const Endpoint& endpoint, const std::wstring& path, const std::vector<unsigned char>& png,
                        std::string& body, std::string& reason) {
  return HttpPostRaw(endpoint, path, png.data(), png.size(), L"image/png", body, reason);
}

}  // namespace dshwork