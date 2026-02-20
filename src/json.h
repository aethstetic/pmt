#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>

namespace pmt {

struct JsonValue;
using JsonPtr = std::shared_ptr<JsonValue>;

using JsonObject = std::map<std::string, JsonPtr>;
using JsonArray = std::vector<JsonPtr>;

struct JsonValue {
    enum Type { Null, Bool, Number, String, Array, Object };

    Type type = Null;
    bool bool_val = false;
    double number_val = 0.0;
    std::string string_val;
    JsonArray array_val;
    JsonObject object_val;

    bool is_null() const { return type == Null; }
    bool is_array() const { return type == Array; }
    bool is_object() const { return type == Object; }

    JsonPtr operator[](const std::string& key) const;
    JsonPtr operator[](size_t index) const;

    std::string str(const std::string& def = "") const;
    double num(double def = 0.0) const;
    int integer(int def = 0) const;

    static JsonPtr make_null();
    static JsonPtr make_bool(bool v);
    static JsonPtr make_number(double v);
    static JsonPtr make_string(const std::string& v);
    static JsonPtr make_array();
    static JsonPtr make_object();
};

class JsonParser {
public:
    JsonPtr parse(const std::string& input);
    std::string error() const { return error_; }

private:
    const char* ptr_ = nullptr;
    const char* end_ = nullptr;
    std::string error_;

    void skip_whitespace();
    JsonPtr parse_value();
    JsonPtr parse_string_value();
    JsonPtr parse_number();
    JsonPtr parse_object();
    JsonPtr parse_array();
    JsonPtr parse_literal();
    std::string parse_string();
    char parse_hex4();
};

}
