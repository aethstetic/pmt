#include "json.h"
#include <cstring>
#include <cstdlib>

namespace pmt {

JsonPtr JsonValue::operator[](const std::string& key) const {
    if (type != Object) return make_null();
    auto it = object_val.find(key);
    if (it == object_val.end()) return make_null();
    return it->second;
}

JsonPtr JsonValue::operator[](size_t index) const {
    if (type != Array || index >= array_val.size()) return make_null();
    return array_val[index];
}

std::string JsonValue::str(const std::string& def) const {
    return type == String ? string_val : def;
}

double JsonValue::num(double def) const {
    return type == Number ? number_val : def;
}

int JsonValue::integer(int def) const {
    return type == Number ? static_cast<int>(number_val) : def;
}

JsonPtr JsonValue::make_null() {
    auto v = std::make_shared<JsonValue>();
    v->type = Null;
    return v;
}

JsonPtr JsonValue::make_bool(bool b) {
    auto v = std::make_shared<JsonValue>();
    v->type = Bool;
    v->bool_val = b;
    return v;
}

JsonPtr JsonValue::make_number(double n) {
    auto v = std::make_shared<JsonValue>();
    v->type = Number;
    v->number_val = n;
    return v;
}

JsonPtr JsonValue::make_string(const std::string& s) {
    auto v = std::make_shared<JsonValue>();
    v->type = String;
    v->string_val = s;
    return v;
}

JsonPtr JsonValue::make_array() {
    auto v = std::make_shared<JsonValue>();
    v->type = Array;
    return v;
}

JsonPtr JsonValue::make_object() {
    auto v = std::make_shared<JsonValue>();
    v->type = Object;
    return v;
}

/* parses JSON string into a JsonValue tree */
JsonPtr JsonParser::parse(const std::string& input) {
    ptr_ = input.c_str();
    end_ = ptr_ + input.size();
    error_.clear();

    skip_whitespace();
    auto result = parse_value();
    if (!result) {
        if (error_.empty()) error_ = "Failed to parse JSON";
        return JsonValue::make_null();
    }
    return result;
}

void JsonParser::skip_whitespace() {
    while (ptr_ < end_ && (*ptr_ == ' ' || *ptr_ == '\t' || *ptr_ == '\r' || *ptr_ == '\n')) {
        ++ptr_;
    }
}

JsonPtr JsonParser::parse_value() {
    if (ptr_ >= end_) {
        error_ = "Unexpected end of input";
        return nullptr;
    }

    switch (*ptr_) {
        case '"': return parse_string_value();
        case '{': return parse_object();
        case '[': return parse_array();
        case 't': case 'f': case 'n': return parse_literal();
        default:
            if (*ptr_ == '-' || (*ptr_ >= '0' && *ptr_ <= '9')) {
                return parse_number();
            }
            error_ = "Unexpected character: ";
            error_ += *ptr_;
            return nullptr;
    }
}

JsonPtr JsonParser::parse_string_value() {
    std::string s = parse_string();
    if (!error_.empty()) return nullptr;
    return JsonValue::make_string(s);
}

std::string JsonParser::parse_string() {
    if (ptr_ >= end_ || *ptr_ != '"') {
        error_ = "Expected '\"'";
        return "";
    }
    ++ptr_;

    std::string result;
    while (ptr_ < end_ && *ptr_ != '"') {
        if (*ptr_ == '\\') {
            ++ptr_;
            if (ptr_ >= end_) { error_ = "Unexpected end in string"; return ""; }
            switch (*ptr_) {
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                case '/': result += '/'; break;
                case 'b': result += '\b'; break;
                case 'f': result += '\f'; break;
                case 'n': result += '\n'; break;
                case 'r': result += '\r'; break;
                case 't': result += '\t'; break;
                case 'u': {
                    ++ptr_;
                    char c = parse_hex4();
                    if (error_.empty()) result += c;
                    else return "";
                    continue;
                }
                default:
                    error_ = "Invalid escape sequence";
                    return "";
            }
        } else {
            result += *ptr_;
        }
        ++ptr_;
    }

    if (ptr_ >= end_) { error_ = "Unterminated string"; return ""; }
    ++ptr_;
    return result;
}

char JsonParser::parse_hex4() {
    if (ptr_ + 4 > end_) { error_ = "Invalid hex escape"; return 0; }
    unsigned val = 0;
    for (int i = 0; i < 4; ++i) {
        char c = ptr_[i];
        if (c >= '0' && c <= '9') val = val * 16 + (c - '0');
        else if (c >= 'a' && c <= 'f') val = val * 16 + (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') val = val * 16 + (c - 'A' + 10);
        else { error_ = "Invalid hex digit"; return 0; }
    }
    ptr_ += 4;
    return static_cast<char>(val & 0x7F);
}

JsonPtr JsonParser::parse_number() {
    const char* start = ptr_;
    if (*ptr_ == '-') ++ptr_;

    while (ptr_ < end_ && *ptr_ >= '0' && *ptr_ <= '9') ++ptr_;
    if (ptr_ < end_ && *ptr_ == '.') {
        ++ptr_;
        while (ptr_ < end_ && *ptr_ >= '0' && *ptr_ <= '9') ++ptr_;
    }
    if (ptr_ < end_ && (*ptr_ == 'e' || *ptr_ == 'E')) {
        ++ptr_;
        if (ptr_ < end_ && (*ptr_ == '+' || *ptr_ == '-')) ++ptr_;
        while (ptr_ < end_ && *ptr_ >= '0' && *ptr_ <= '9') ++ptr_;
    }

    std::string numstr(start, ptr_);
    double val = std::strtod(numstr.c_str(), nullptr);
    return JsonValue::make_number(val);
}

JsonPtr JsonParser::parse_object() {
    if (ptr_ >= end_ || *ptr_ != '{') { error_ = "Expected '{'"; return nullptr; }
    ++ptr_;
    skip_whitespace();

    auto obj = JsonValue::make_object();

    if (ptr_ < end_ && *ptr_ == '}') { ++ptr_; return obj; }

    while (ptr_ < end_) {
        skip_whitespace();
        if (ptr_ >= end_ || *ptr_ != '"') { error_ = "Expected string key"; return nullptr; }
        std::string key = parse_string();
        if (!error_.empty()) return nullptr;

        skip_whitespace();
        if (ptr_ >= end_ || *ptr_ != ':') { error_ = "Expected ':'"; return nullptr; }
        ++ptr_;
        skip_whitespace();

        auto val = parse_value();
        if (!val) return nullptr;

        obj->object_val[key] = val;

        skip_whitespace();
        if (ptr_ < end_ && *ptr_ == ',') {
            ++ptr_;
            continue;
        }
        break;
    }

    skip_whitespace();
    if (ptr_ >= end_ || *ptr_ != '}') { error_ = "Expected '}'"; return nullptr; }
    ++ptr_;
    return obj;
}

JsonPtr JsonParser::parse_array() {
    if (ptr_ >= end_ || *ptr_ != '[') { error_ = "Expected '['"; return nullptr; }
    ++ptr_;
    skip_whitespace();

    auto arr = JsonValue::make_array();

    if (ptr_ < end_ && *ptr_ == ']') { ++ptr_; return arr; }

    while (ptr_ < end_) {
        skip_whitespace();
        auto val = parse_value();
        if (!val) return nullptr;
        arr->array_val.push_back(val);

        skip_whitespace();
        if (ptr_ < end_ && *ptr_ == ',') {
            ++ptr_;
            continue;
        }
        break;
    }

    skip_whitespace();
    if (ptr_ >= end_ || *ptr_ != ']') { error_ = "Expected ']'"; return nullptr; }
    ++ptr_;
    return arr;
}

JsonPtr JsonParser::parse_literal() {
    if (end_ - ptr_ >= 4 && std::strncmp(ptr_, "true", 4) == 0) {
        ptr_ += 4;
        return JsonValue::make_bool(true);
    }
    if (end_ - ptr_ >= 5 && std::strncmp(ptr_, "false", 5) == 0) {
        ptr_ += 5;
        return JsonValue::make_bool(false);
    }
    if (end_ - ptr_ >= 4 && std::strncmp(ptr_, "null", 4) == 0) {
        ptr_ += 4;
        return JsonValue::make_null();
    }
    error_ = "Invalid literal";
    return nullptr;
}

}
