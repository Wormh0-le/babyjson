#include <variant>
#include <vector>
#include <unordered_map>
#include <string>
#include <regex>
#include <charconv>
#include <codecvt>
#include "print.h"

struct JSONObject;

using JSONDict = std::unordered_map<std::string, JSONObject>;
using JSONList = std::vector<JSONObject>;

struct JSONObject {
    std::variant<
       std::nullptr_t,
       bool,
       int,
       double,
       std::string,
       std::vector<JSONObject>, // 固定24字节，只需要包含JSONObject的指针，不需要JSONObject是完整定义的，这里是存在栈上的
       std::unordered_map<std::string, JSONObject>
    > inner;
    
    void do_print() const {
        printnl(inner);
    }

    template <class T>
    bool is() const {
        return std::holds_alternative<T>(inner);
    }

    template <class T>
    T const &get() const {
        return std::get<T>(inner);
    }

    template <class T>
    T &get() {
        return std::get<T>(inner);
    }
};

template<class T>
std::optional<T> try_parse_num(std::string_view str) {
    T value;
    auto res = std::from_chars(str.data(), str.data()+str.size(), value);
    if(res.ec == std::errc() && res.ptr == str.data() + str.size()) {
        return value;
    }
    return std::nullopt;
}

char unescaped_char(char c) {
    switch (c) {
    case 'n': return '\n';
    case 'r': return '\r';
    case '0': return '\0';
    case 't': return '\t';
    case 'v': return '\v';
    case 'f': return '\f';
    case 'b': return '\b';
    case 'a': return '\a';
    default: return c;
    }
}

// 辅助函数：将 Unicode 码点转换为 UTF-8 编码
std::string utf8Encode(uint32_t codepoint) {
    std::string result;
    if (codepoint <= 0x7F) {    // 对于 Unicode 范围在 0 至 127 的码点，UTF-8 与 ASCII 保持一致，只需一个字节直接表示码点的值。
        result.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {    // 对于 128 到 2047 之间的码点，UTF-8 编码需要两个字节。
        /*第一字节的格式为 110xxxxx。第二字节的格式为 10xxxxxx。实现细节：
            第一字节中，使用 0xC0（二进制 11000000）作为固定前缀，然后取码点的高 5 位（(codepoint >> 6) 后用 & 0x1F 保留低 5 位）。
            第二字节中，使用 0x80（二进制 10000000）作为固定前缀，然后取码点的低 6 位（codepoint & 0x3F）
        */
        result.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
        result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) { //对于码点在 2048 至 65535 范围内，UTF-8 需要三个字节进行编码。
        /* 第一字节格式：1110xxxx，第二字节格式：10xxxxxx，第三字节格式：10xxxxxx实现细节：
            第一字节：使用 0xE0（二进制 11100000）作为前缀，并将码点的高 4 位（codepoint >> 12 后用 & 0x0F 保留低 4 位）填入。
            第二字节：前缀为 0x80，填入码点中间的 6 位（(codepoint >> 6) & 0x3F）。
            第三字节：同样前缀为 0x80，填入码点的最低 6 位（codepoint & 0x3F）。
        */
        result.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
        result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0x10FFFF) { // 对于码点在 65536 到 1114111（0x10FFFF）范围内，需要四个字节。
       /*字节结构：第一字节格式：11110xxx，第二、三、四字节格式：10xxxxxx 实现细节：
           第一字节：使用 0xF0（二进制 11110000）作为前缀，再将码点的最高 3 位（codepoint >> 18 后用 & 0x07 保留低 3 位）填入。
           第二字节：取码点接下来的 6 位（(codepoint >> 12) & 0x3F），加上前缀 0x80。
           第三字节：取码点中间的 6 位（(codepoint >> 6) & 0x3F），加上前缀 0x80。
           第四字节：取码点的最低 6 位（codepoint & 0x3F），加上前缀 0x80。
       */
        result.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
        result.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        throw std::runtime_error("Invalid Unicode code point");
    }
    return result;
}

std::pair<JSONObject, size_t> parse(std::string_view json) {
    if (json.empty()) {
        return {JSONObject{std::nullptr_t{}}, 0};
    } else if(size_t off = json.find_first_not_of(" \n\r\t\v\f\0"); off != 0 && off != json.npos) {
        auto [obj, eaten] = parse(json.substr(off));
        return {std::move(obj), eaten + off};
    } else if ('0' <= json[0] && json[0] <= '9' || json[0] == '+' || json[0] == '-') {
        std::regex num_re{"[+-]?[0-9]+(\\.[0-9]*)?([eE][+-]?[0-9]+)?"};
        std::cmatch match;
        if(std::regex_search(json.data(), json.data()+json.size(), match, num_re)) {
            std::string str = match.str();
            if (auto num = try_parse_num<int>(str); num.has_value()) {
                return {JSONObject{num.value()}, str.size()};
            } else if(auto num = try_parse_num<double>(str)) {
                return {JSONObject{num.value()}, str.size()};
            }
        }
    } else if(json[0] == 't' || json[0] == 'f' || json[0] == 'n' || json[0] == 'T' || json[0] == 'F' || json[0] == 'N') {
        std::regex bool_re{"(true|false|null|True|False|Null)"};
        std::cmatch match;
        if (std::regex_search(json.data(), json.data() + json.size(), match, bool_re)) {
            std::string str = match.str();
            if (str == "true" || str == "True") {
                return {JSONObject{true}, str.size()};
            } else if (str == "false" || str == "False") {
                return {JSONObject{false}, str.size()};
            } else if (str == "null" || str == "Null") {
                return {JSONObject{std::nullptr_t{}}, str.size()};
            }
        }
    } else if (json[0] == '"' || json[0] == '\'') {
        char quote = json[0];
        std::string str;
        enum {
            Raw,
            Escaped,
            Hex1,
            Hex2,
            Hex4
        } phase = Raw;
        size_t i;
        for(i = 1; i < json.size(); i++) {
            char ch = json[i];
            if (phase == Raw) {
                if(ch == '\\') {
                    phase = Escaped;
                } else if(ch == quote) {
                    i += 1;
                    break;
                } else {
                    str += ch;
                }
            } else if (phase == Escaped) {
                if (ch == 'x') {
                    phase = Hex1;
                } else if (ch == 'u') {
                    phase = Hex2;
                } else if (ch == 'U') {
                    phase = Hex4;
                } else {
                    str += unescaped_char(ch);
                    phase = Raw;
                }
            } else if(phase == Hex1) {
                std::string hexVal = json.substr(i, 2).data();
                uint32_t code = std::stoul(hexVal, nullptr, 16);
                str += utf8Encode(code);
                // std::wstring_convert<std::codecvt_utf8<wchar_t>> ucs1conv;
                // str += ucs1conv.to_bytes(static_cast<wchar_t>(std::stoul(std::string{json.substr(i, 2)}, nullptr, 16)));
                phase = Raw;
                i += 1;
            } else if(phase == Hex2) {
                std::string hexVal = json.substr(i, 4).data();
                uint32_t code = std::stoul(hexVal, nullptr, 16);
                i += 3;
                // 处理两个ucs2拼接的情况，如果后面还有两个字符，且是\u开头的，那么有可能是低代理
                if (code >= 0xD800 && code <= 0xDBFF) { // 高代理
                    if (i + 6 < json.size() && json[i+1] == '\\' && json[i + 2] == 'u') {
                        std::string hexVal2 = json.substr(i+3, 4).data();
                        uint32_t low = std::stoul(hexVal2, nullptr, 16);
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            code = ((code - 0xD800) << 10) + (low - 0xDC00) + 0x10000;
                            i += 5;
                        }
                    }
                }
                str += utf8Encode(code);
                // std::wstring_convert<std::codecvt_utf8<wchar_t>> ucs2conv;
                // str += ucs2conv.to_bytes(static_cast<wchar_t>(std::stoul(std::string{json.substr(i, 4)}, nullptr, 16)));
                phase = Raw;
                
            } else if(phase == Hex4) {
                std::string hexVal = json.substr(i, 8).data();
                uint32_t code = std::stoul(hexVal, nullptr, 16);
                str += utf8Encode(code);
                // std::wstring_convert<std::codecvt_utf8<wchar_t>> ucs4conv;
                // str += ucs4conv.to_bytes(static_cast<wchar_t>(std::stoul(std::string{json.substr(i, 8)}, nullptr, 16)));
                phase = Raw;
                i += 7;
            }
        }
        return {JSONObject{std::move(str)}, i};
    } else if(json[0] == '[') {
        std::vector<JSONObject> res;
        size_t i;
        for(i = 1; i < json.size();) {
            if (json[i] == ']') {
                i += 1;
                break;
            }
            auto [obj, eaten] = parse(json.substr(i));
            if (eaten == 0) {
                i = 0;
                break;
            }
            res.push_back(std::move(obj));
            i += eaten;
            if (json[i] == ',') {
                i += 1;
            }
        }
        return {JSONObject{std::move(res)}, i};
    } else if(json[0] == '{') {
        std::unordered_map<std::string, JSONObject> res;
        size_t i;
        for(i = 1; i < json.size();) {
            if (json[i] == '}') {
                i += 1;
                break;
            }
            auto [keyobj, keyeaten] = parse(json.substr(i));
            if (keyeaten == 0) {
                i = 0;
                break;
            }
            i += keyeaten;
            if(!std::holds_alternative<std::string>(keyobj.inner)) {
                i = 0;
                break;
            }
            if (json[i] == ':') {
                i += 1;
            } 
            std::string key = std::move(std::get<std::string>(keyobj.inner));
            auto [valobj, valeaten] = parse(json.substr(i));
            if(valeaten == 0) {
                i = 0;
                break;
            }
            i += valeaten;
            res.try_emplace(std::move(key), std::move(valobj));
            if (json[i] == ',') {
                i += 1;
            }
        }
        return {JSONObject{std::move(res)}, i};
    }
    return {JSONObject{std::nullptr_t{}}, 0};
}

std::string dump(const JSONObject& obj, bool isPretty = false, int depth = 0) {
    const std::string indentUnit = "  "; // 每层缩进两个空格
    std::string currentIndent = isPretty ? std::string(depth * indentUnit.size(), ' ') : "";
    std::string nextIndent = isPretty ? std::string((depth + 1) * indentUnit.size(), ' ') : "";

    if (std::holds_alternative<std::nullptr_t>(obj.inner)) {
        return "null";
    } else if (std::holds_alternative<int>(obj.inner)) {
        return std::to_string(std::get<int>(obj.inner));
    } else if (std::holds_alternative<double>(obj.inner)) {
        return std::to_string(std::get<double>(obj.inner));
    } else if (std::holds_alternative<std::string>(obj.inner)) {
        return std::get<std::string>(obj.inner);
    } else if (std::holds_alternative<std::vector<JSONObject>>(obj.inner)) {
        const auto& arr = std::get<std::vector<JSONObject>>(obj.inner);
        if (arr.empty()) return "[]";

        std::string res = "[";
        if (isPretty) res += "\n";
        for (size_t i = 0; i < arr.size(); ++i) {
            if (isPretty) res += nextIndent;
            res += dump(arr[i], isPretty, depth + 1);
            if (i != arr.size() - 1) res += ",";
            if (isPretty) res += "\n";
        }
        if (isPretty) res += currentIndent;
        res += "]";
        return res;
    } else if (std::holds_alternative<std::unordered_map<std::string, JSONObject>>(obj.inner)) {
        const auto& map = std::get<std::unordered_map<std::string, JSONObject>>(obj.inner);
        if (map.empty()) return "{}";

        std::string res = "{";
        if (isPretty) res += "\n";
        size_t count = 0;
        for (const auto& [key, val] : map) {
            if (isPretty) res += nextIndent;
            res += key + ": ";
            res += dump(val, isPretty, depth + 1);
            if (count != map.size() - 1) res += ",";
            if (isPretty) res += "\n";
            ++count;
        }
        if (isPretty) res += currentIndent;
        res += "}";
        return res;
    } else {
        return "\"unknown\"";
    }
}


template <class ...Fs>
struct overloaded : Fs... {
    using Fs::operator()...;
};

template <class ...Fs>
overloaded(Fs...) -> overloaded<Fs...>;

int main() {
    std::string_view str3 = R"JSON({"hello": 3.14, "world": [211, [985, 211], '\x36', '\u4E2D', '\U0001F600']})JSON";
    // std::string_view str3 = R"JSON(['\xAD', '\u4E2D', '\uD83D\uDE01', '\U0001F600'])JSON";
    auto [obj, eaten] = parse(str3);
    std::visit(
        overloaded{
            [&] (int val) {
                print(val);
            },
            [&] (double val) {
                print(val);
            },
            [&] (std::string val) {
                print(val);
            },
            [&] (JSONObject val) {
                print(val);
            },
            [&] (JSONList val) {
                print(val);
            },
            [&] (JSONDict val) {
                print(val);
            },
        },
        obj.inner
    );
    print(dump(obj, true));
    return 0;
}