#include "request.h"
#include <format>
#include <charconv>

using namespace std;

namespace bt {
    // Format: "bt1\n<kind>\n<hwnd hex>\n<modifiers>\n<start>\n<main>\n<url>". The URL goes last, so it may hold anything.
    static constexpr string_view magic{"bt1"};

    string request::serialize() const {
        return format("{}\n{}\n{:x}\n{}\n{}\n{}\n{}", magic, static_cast<int>(kind), source_hwnd, modifiers,
            launcher_start_100ns, launcher_main_100ns, url);
    }

    optional<request> request::deserialize(const string& s) {
        string_view rest{s};
        auto next = [&rest](string_view& out) {
            size_t pos = rest.find('\n');
            if(pos == string_view::npos) return false;
            out = rest.substr(0, pos);
            rest.remove_prefix(pos + 1);
            return true;
        };
        auto to_num = [](string_view v, auto& out, int base = 10) {
            return from_chars(v.data(), v.data() + v.size(), out, base).ec == errc{};
        };

        string_view f_magic, f_kind, f_hwnd, f_mods, f_start, f_main;
        if(!next(f_magic) || f_magic != magic) return nullopt;
        if(!next(f_kind) || !next(f_hwnd) || !next(f_mods) || !next(f_start) || !next(f_main)) return nullopt;

        request r;
        int kind{0};
        if(!to_num(f_kind, kind) || kind < 0 || kind > static_cast<int>(request_kind::exit)) return nullopt;
        r.kind = static_cast<request_kind>(kind);
        to_num(f_hwnd, r.source_hwnd, 16);
        to_num(f_mods, r.modifiers);
        to_num(f_start, r.launcher_start_100ns);
        to_num(f_main, r.launcher_main_100ns);
        r.url = string{rest};
        return r;
    }
}
