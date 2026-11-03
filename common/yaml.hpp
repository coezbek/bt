#pragma once
// YAML read/write helpers, taken from grey's config.hpp (MIT, see LICENSE-grey).
#include <string>
#include <format>
#include <fkYAML/node.hpp>
#include "colour.h"
#include <type_traits>
#include "magic_enum/magic_enum.hpp"

namespace grey::common {

    template<typename T>
    bool read(const fkyaml::node& node, const std::string& key, T& value_ref) {
        if(!node.contains(key)) return false;

        const fkyaml::node &value = node[key];

        try {
            value_ref = value.get_value<T>();
            return true;
        } catch(const fkyaml::exception&) {
            // wrong type, or conversion failed
        }
        return false;
    }

    template<typename T>
    static void write(fkyaml::node &n, const std::string &key, const T &value) {
        n[key] = value;
    }

    template<typename T>
    bool read_enum(const fkyaml::node& node, const std::string& key, T& value_ref) {
        if(!node.contains(key)) return false;
        
        static_assert(std::is_enum_v<T>, "T must be an enum");

        const fkyaml::node &value = node[key];

        try {
            auto str_val = value.get_value<std::string>();
            auto enum_val = magic_enum::enum_cast<T>(str_val);
            if (enum_val.has_value()) {
                value_ref = enum_val.value();
                return true;
            }
        } catch(const fkyaml::exception&) {
            // Not a string, or magic_enum failed
        }
        return false;
    }

    template<typename T>
    static void write_enum(fkyaml::node &n, const std::string &key, const T &value) {
        static_assert(std::is_enum_v<T>, "T must be an enum");
        n[key] = std::string(magic_enum::enum_name(value));
    }

    inline bool read_color(const fkyaml::node& node, const std::string& key, grey::rgb_colour& value_ref) {
        std::string hex;
        if(read<std::string>(node, key, hex)) {
            value_ref = grey::rgb_colour(hex);
            return true;
        }
        return false;
    }

    inline void write_color(fkyaml::node &n, const std::string& key, const grey::rgb_colour& value_ref) {
        n[key] = value_ref.to_hex(false);
    }

}
