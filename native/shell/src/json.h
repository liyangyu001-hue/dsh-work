// 极简 JSON 解析器（header-only）
//
// 为什么自己写而不引第三方：原生层要读的东西只有形状固定的两样——
// endpoint.json 和宿主的状态文档。引一个库就是多一个要同步维护的东西，
// 而 C++ 的包管理在 Windows 上恰恰是最麻烦的那一环。
//
// 支持完整 JSON 语法（对象/数组/字符串/数字/true/false/null、\u 转义），
// 不做流式、不做注释、不保证错误恢复——解析失败就是解析失败，调用方自己兜底。

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dshwork {

struct JValue;
using JObject = std::map<std::string, JValue>;
using JArray = std::vector<JValue>;

struct JValue {
  enum class Type { Null, Bool, Number, String, Object, Array } type = Type::Null;
  bool boolean = false;
  double number = 0.0;
  std::string text;
  JObject object;
  JArray array;

  bool IsNull() const { return type == Type::Null; }
  bool IsObject() const { return type == Type::Object; }
  bool IsArray() const { return type == Type::Array; }
  bool IsString() const { return type == Type::String; }

  /** 取对象成员，不存在返回 nullptr（比抛异常好用在"结构可能变"的场合）。 */
  const JValue* Get(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    auto it = object.find(key);
    return it == object.end() ? nullptr : &it->second;
  }

  std::string GetString(const std::string& key, const std::string& fallback = {}) const {
    const JValue* value = Get(key);
    return (value != nullptr && value->IsString()) ? value->text : fallback;
  }
};

namespace detail {

inline void AppendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

class Parser {
 public:
  explicit Parser(const std::string& input) : input_(input) {}

  bool Parse(JValue& out) {
    SkipSpace();
    if (!ParseValue(out, 0)) return false;
    SkipSpace();
    return pos_ == input_.size();
  }

 private:
  static constexpr int kMaxDepth = 64;

  void SkipSpace() {
    while (pos_ < input_.size()) {
      char c = input_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  bool Literal(const char* word) {
    size_t len = 0;
    while (word[len] != '\0') ++len;
    if (input_.compare(pos_, len, word) != 0) return false;
    pos_ += len;
    return true;
  }

  bool ParseValue(JValue& out, int depth) {
    if (depth > kMaxDepth || pos_ >= input_.size()) return false;
    switch (input_[pos_]) {
      case '{': return ParseObject(out, depth);
      case '[': return ParseArray(out, depth);
      case '"': {
        out.type = JValue::Type::String;
        return ParseString(out.text);
      }
      case 't':
        if (!Literal("true")) return false;
        out.type = JValue::Type::Bool;
        out.boolean = true;
        return true;
      case 'f':
        if (!Literal("false")) return false;
        out.type = JValue::Type::Bool;
        out.boolean = false;
        return true;
      case 'n':
        if (!Literal("null")) return false;
        out.type = JValue::Type::Null;
        return true;
      default: return ParseNumber(out);
    }
  }

  bool ParseObject(JValue& out, int depth) {
    out.type = JValue::Type::Object;
    ++pos_;  // '{'
    SkipSpace();
    if (pos_ < input_.size() && input_[pos_] == '}') {
      ++pos_;
      return true;
    }
    while (true) {
      SkipSpace();
      std::string key;
      if (pos_ >= input_.size() || input_[pos_] != '"') return false;
      if (!ParseString(key)) return false;
      SkipSpace();
      if (pos_ >= input_.size() || input_[pos_] != ':') return false;
      ++pos_;
      SkipSpace();
      JValue value;
      if (!ParseValue(value, depth + 1)) return false;
      out.object.emplace(std::move(key), std::move(value));
      SkipSpace();
      if (pos_ >= input_.size()) return false;
      if (input_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (input_[pos_] == '}') {
        ++pos_;
        return true;
      }
      return false;
    }
  }

  bool ParseArray(JValue& out, int depth) {
    out.type = JValue::Type::Array;
    ++pos_;  // '['
    SkipSpace();
    if (pos_ < input_.size() && input_[pos_] == ']') {
      ++pos_;
      return true;
    }
    while (true) {
      SkipSpace();
      JValue value;
      if (!ParseValue(value, depth + 1)) return false;
      out.array.push_back(std::move(value));
      SkipSpace();
      if (pos_ >= input_.size()) return false;
      if (input_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (input_[pos_] == ']') {
        ++pos_;
        return true;
      }
      return false;
    }
  }

  bool ParseString(std::string& out) {
    ++pos_;  // '"'
    out.clear();
    while (pos_ < input_.size()) {
      const char c = input_[pos_++];
      if (c == '"') return true;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos_ >= input_.size()) return false;
      const char esc = input_[pos_++];
      switch (esc) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          if (pos_ + 4 > input_.size()) return false;
          uint32_t cp = 0;
          for (int i = 0; i < 4; ++i) {
            const char h = input_[pos_++];
            cp <<= 4;
            if (h >= '0' && h <= '9') {
              cp |= static_cast<uint32_t>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
              cp |= static_cast<uint32_t>(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
              cp |= static_cast<uint32_t>(h - 'A' + 10);
            } else {
              return false;
            }
          }
          // 代理对：高位后面必须紧跟 \uDCxx，合成一个码点再转 UTF-8
          if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= input_.size() &&
              input_[pos_] == '\\' && input_[pos_ + 1] == 'u') {
            pos_ += 2;
            uint32_t low = 0;
            for (int i = 0; i < 4; ++i) {
              const char h = input_[pos_++];
              low <<= 4;
              if (h >= '0' && h <= '9') {
                low |= static_cast<uint32_t>(h - '0');
              } else if (h >= 'a' && h <= 'f') {
                low |= static_cast<uint32_t>(h - 'a' + 10);
              } else if (h >= 'A' && h <= 'F') {
                low |= static_cast<uint32_t>(h - 'A' + 10);
              } else {
                return false;
              }
            }
            if (low >= 0xDC00 && low <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            }
          }
          AppendUtf8(out, cp);
          break;
        }
        default: return false;
      }
    }
    return false;
  }

  bool ParseNumber(JValue& out) {
    const size_t begin = pos_;
    if (pos_ < input_.size() && (input_[pos_] == '-' || input_[pos_] == '+')) ++pos_;
    bool digits = false;
    while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') {
      ++pos_;
      digits = true;
    }
    if (pos_ < input_.size() && input_[pos_] == '.') {
      ++pos_;
      while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') {
        ++pos_;
        digits = true;
      }
    }
    if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < input_.size() && (input_[pos_] == '-' || input_[pos_] == '+')) ++pos_;
      while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') ++pos_;
    }
    if (!digits) return false;
    out.type = JValue::Type::Number;
    out.number = std::stod(input_.substr(begin, pos_ - begin));
    return true;
  }

  const std::string& input_;
  size_t pos_ = 0;
};

}  // namespace detail

inline bool ParseJson(const std::string& input, JValue& out) {
  detail::Parser parser(input);
  return parser.Parse(out);
}

}  // namespace dshwork
