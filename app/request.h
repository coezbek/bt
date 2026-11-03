#pragma once
#include <string>
#include <cstdint>
#include <optional>

namespace bt {

    /**
     * @brief What bt.exe asks the resident process to do.
     */
    enum class request_kind : int {
        open = 0,       // route a URL by the rules, maybe show the picker
        pick = 1,       // always show the picker
        config = 2,     // show the settings window
        discover = 3,   // rediscover browsers and save
        background = 4, // only make sure the resident process runs
        exit = 5        // stop the resident process
    };

    enum request_modifiers : uint32_t {
        mod_none = 0,
        mod_shift = 1,
        mod_ctrl = 2,
        mod_alt = 4,
        mod_caps = 8
    };

    /**
     * @brief A click as captured by the short-lived bt.exe and sent to the resident process.
     *
     * The launcher captures what only it can know: the window the click came from and the
     * modifier keys at the time of the click. It also sends its own start time so the resident
     * process can measure click-to-picker latency.
     */
    struct request {
        request_kind kind{request_kind::open};
        std::string url;
        uintptr_t source_hwnd{0};
        uint32_t modifiers{mod_none};
        uint64_t launcher_start_100ns{0};   // FILETIME of launcher process creation, 0 if unknown
        uint64_t launcher_main_100ns{0};    // FILETIME when the launcher reached wWinMain

        [[nodiscard]] std::string serialize() const;
        static std::optional<request> deserialize(const std::string& s);
    };
}
