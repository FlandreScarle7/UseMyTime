/*
 * UseMyTime - MiniJson
 *
 * 极简 JSON 工具（单头文件，无第三方依赖）：
 *   - 解析扁平 JSON 对象（顶层对象 + string/number/bool 值）
 *   - 构造扁平 JSON 对象
 *   - 满足 IPC 命令/响应的最小需求，不做完整 JSON 支持
 */
#pragma once

#include <string>
#include <map>
#include <cstdlib>
#include <cstdio>
#include <cctype>
#include <cmath>

namespace usemytime {

class MiniJson {
public:
    struct Value {
        enum Type { None, String, Number, Bool } type = None;
        std::string str;
        double      num = 0.0;
        bool        boolean = false;

        std::string AsString() const { return type == String ? str : ""; }
        double      AsNumber(double def = 0.0) const
        { return type == Number ? num : def; }
        long long   AsInt(long long def = 0) const
        { return type == Number ? static_cast<long long>(num) : def; }
        bool        AsBool(bool def = false) const
        { return type == Bool ? boolean : def; }
    };

    // ------------------------------------------------------------------
    // 解析
    // ------------------------------------------------------------------
    static MiniJson Parse(const std::string& text)
    {
        MiniJson j;
        size_t i = 0;
        SkipWs(text, i);
        if (i >= text.size() || text[i] != '{') return j; // 非对象 -> 空
        ++i;
        while (true) {
            SkipWs(text, i);
            if (i >= text.size()) break;
            if (text[i] == '}') { ++i; break; }
            if (text[i] == ',') { ++i; continue; }

            // key
            if (text[i] != '"') break;
            std::string key = ParseString(text, i);
            SkipWs(text, i);
            if (i < text.size() && text[i] == ':') ++i;
            SkipWs(text, i);

            Value v;
            if (i < text.size() && text[i] == '"') {
                v.type = Value::String;
                v.str = ParseString(text, i);
            }
            else if (i < text.size() && text[i] == 't') {
                if (text.compare(i, 4, "true") == 0) {
                    v.type = Value::Bool; v.boolean = true; i += 4;
                }
            }
            else if (i < text.size() && text[i] == 'f') {
                if (text.compare(i, 5, "false") == 0) {
                    v.type = Value::Bool; v.boolean = false; i += 5;
                }
            }
            else if (i < text.size() &&
                     (text[i] == '-' || isdigit(static_cast<unsigned char>(text[i])))) {
                v.type = Value::Number;
                char* end = nullptr;
                v.num = strtod(text.c_str() + i, &end);
                i = (end == text.c_str() + i) ? i + 1
                                              : static_cast<size_t>(end - text.c_str());
            }
            else {
                // null / 不支持的类型 -> 跳过到逗号或右括号
                while (i < text.size() && text[i] != ',' && text[i] != '}') ++i;
                continue;
            }
            j.m_values[key] = v;
        }
        return j;
    }

    // ------------------------------------------------------------------
    // 取值
    // ------------------------------------------------------------------
    const Value& Get(const std::string& key) const
    {
        auto it = m_values.find(key);
        return (it == m_values.end()) ? s_empty : it->second;
    }

    bool Has(const std::string& key) const
    { return m_values.find(key) != m_values.end(); }

    // ------------------------------------------------------------------
    // 构造
    // ------------------------------------------------------------------
    void SetString(const std::string& key, const std::string& v)
    {
        Value x; x.type = Value::String; x.str = v;
        m_values[key] = x;
    }
    void SetNumber(const std::string& key, double v)
    {
        Value x; x.type = Value::Number; x.num = v;
        m_values[key] = x;
    }
    void SetBool(const std::string& key, bool v)
    {
        Value x; x.type = Value::Bool; x.boolean = v;
        m_values[key] = x;
    }

    std::string Dump() const
    {
        std::string out = "{";
        bool first = true;
        for (const auto& kv : m_values) {
            if (!first) out += ",";
            first = false;
            out += "\"" + Escape(kv.first) + "\":";
            const Value& v = kv.second;
            switch (v.type) {
                case Value::String: out += "\"" + Escape(v.str) + "\""; break;
                case Value::Number: {
                    char buf[32];
                    if (v.num == static_cast<long long>(v.num))
                        snprintf(buf, sizeof(buf), "%lld",
                                 static_cast<long long>(v.num));
                    else
                        snprintf(buf, sizeof(buf), "%g", v.num);
                    out += buf;
                    break;
                }
                case Value::Bool:   out += v.boolean ? "true" : "false"; break;
                default:            out += "null"; break;
            }
        }
        out += "}";
        return out;
    }

    // 标准状态响应（供 IPC 使用）
    static std::string StatusResponse();

private:
    std::map<std::string, Value> m_values;
    static const Value s_empty;

    static void SkipWs(const std::string& t, size_t& i)
    {
        while (i < t.size() && isspace(static_cast<unsigned char>(t[i]))) ++i;
    }

    static std::string ParseString(const std::string& t, size_t& i)
    {
        std::string out;
        if (i >= t.size() || t[i] != '"') return out;
        ++i;
        while (i < t.size() && t[i] != '"') {
            if (t[i] == '\\' && i + 1 < t.size()) {
                char e = t[i + 1];
                switch (e) {
                    case 'n': out += '\n'; i += 2; break;
                    case 't': out += '\t'; i += 2; break;
                    case 'r': out += '\r'; i += 2; break;
                    case '"': out += '"';  i += 2; break;
                    case '\\': out += '\\'; i += 2; break;
                    default:  out += e;    i += 2; break;
                }
            }
            else {
                out += t[i++];
            }
        }
        if (i < t.size()) ++i; // 跳过结尾引号
        return out;
    }

    static std::string Escape(const std::string& in)
    {
        std::string out;
        out.reserve(in.size());
        for (char c : in) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\t': out += "\\t";  break;
                case '\r': out += "\\r";  break;
                default:   out += c;      break;
            }
        }
        return out;
    }
};

} // namespace usemytime
