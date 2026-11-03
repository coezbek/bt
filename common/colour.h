#pragma once
#include <string>
#include <format>
#include <algorithm>

namespace grey {
    /**
     * @brief RGBA colour with float channels in 0..1. Replaces grey's ImGui-based rgb_colour.
     */
    class rgb_colour {
    public:
        float r{0}, g{0}, b{0}, o{0};

        rgb_colour() = default;

        rgb_colour(float r, float g, float b, float o = 1.0f) : r{r}, g{g}, b{b}, o{o} {}

        /**
         * @brief Parses "RRGGBB" or "RRGGBBAA", with or without a leading '#'.
         */
        explicit rgb_colour(const std::string& hex) {
            std::string h = (!hex.empty() && hex[0] == '#') ? hex.substr(1) : hex;
            o = 1.0f;
            try {
                if(h.length() >= 6) {
                    unsigned long v = std::stoul(h.substr(0, 6), nullptr, 16);
                    r = ((v >> 16) & 0xFF) / 255.0f;
                    g = ((v >> 8) & 0xFF) / 255.0f;
                    b = (v & 0xFF) / 255.0f;
                }
                if(h.length() >= 8) {
                    o = std::stoul(h.substr(6, 2), nullptr, 16) / 255.0f;
                }
            } catch(...) {
            }
        }

        static rgb_colour from_bytes(unsigned char r, unsigned char g, unsigned char b, unsigned char a = 255) {
            return {r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
        }

        [[nodiscard]] unsigned char r8() const { return to8(r); }
        [[nodiscard]] unsigned char g8() const { return to8(g); }
        [[nodiscard]] unsigned char b8() const { return to8(b); }
        [[nodiscard]] unsigned char a8() const { return to8(o); }

        [[nodiscard]] std::string to_hex(bool prepend_hash) const {
            std::string hex = std::format("{}{:02X}{:02X}{:02X}", prepend_hash ? "#" : "", r8(), g8(), b8());
            if(o < 1.0f) hex += std::format("{:02X}", a8());
            return hex;
        }

        bool operator==(const rgb_colour& other) const = default;

    private:
        static unsigned char to8(float v) {
            return static_cast<unsigned char>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    };
}
