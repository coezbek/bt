#pragma once
#include <string>
#include <vector>
#include "model.h"
#include <filesystem>
#include "common/yaml.hpp"
#include "app/browser.h"

namespace bt {
    class toast_state {
    public:
        bool enabled{true};
        int visible_seconds{5};
        int border_width{1};
        float opacity{0.9};
        float anim_duration{0.5f};

        bool operator==(const toast_state &) const = default;
    };

    void to_node(fkyaml::node& node, const toast_state& state);

    void from_node(const fkyaml::node& node, toast_state& state);

    class picker_invoke_state {
    public:
        bool on_key_control_shift{false};
        bool on_key_control_alt{false};
        bool on_key_alt_shift{false};
        bool on_key_caps_locks{false};
        bool on_no_rule{false};

        bool operator==(const picker_invoke_state &) const = default;
    };

    void to_node(fkyaml::node& node, const picker_invoke_state& state);

    void from_node(const fkyaml::node& node, picker_invoke_state& state);

    class picker_state {
    public:
        // box size, not including padding
        float box_size{60.0f};
        // padding between boxes
        float item_padding{15.0f};
        float item_rounding{10.0f};
        bool show_key_hints{true};
        int border_width{1};
        bool show_native_chrome{false};
        float label_size{-4.0f};
        float opacity{1};
        bool close_on_focus_loss{true};
        bool always_on_top{false};
        int min_width_perc{20};
        int max_width_perc{90};
        label_display_mode label_display{label_display_mode::browser_and_profile};

        picker_invoke_state invoke;

        bool operator==(const picker_state &) const = default;
    };

    void to_node(fkyaml::node& node, const picker_state& state);

    void from_node(const fkyaml::node& node, picker_state& state);

    class substitition_state {
    public:
        std::string kind;
        std::string find;
        std::string replace;

        bool operator==(const substitition_state &) const = default;
    };

    void to_node(fkyaml::node& node, const substitition_state& state);

    void from_node(const fkyaml::node& node, substitition_state& state);

    class transforms_state {
    public:
        bool unwrap_o365{true};
        bool unshorten{true};
        bool substitute{true};
        bool clearurls{true};
        bool scripting{true};
        std::vector<substitition_state> substitutions;

        bool operator==(const transforms_state &) const = default;
    };

    void to_node(fkyaml::node &node, const transforms_state &state);

    void from_node(const fkyaml::node& node, transforms_state& state);

    class pipevis_state {
    public:
        std::string url;
        std::string window_title;
        std::string process_name;

        bool operator==(const pipevis_state &) const = default;
    };

    void to_node(fkyaml::node &node, const pipevis_state &state);

    void from_node(const fkyaml::node& node, pipevis_state& state);

    void from_node(const fkyaml::node& node, browser_profile& state);

    void to_node(fkyaml::node &node, const browser_profile &state);

    void from_node(const fkyaml::node& node, browser& state);

    void to_node(fkyaml::node &node, const browser &state);

    class state {
    public:
        std::string ui_theme;
        bool log_rule_hits{false};
        bool show_hidden_browsers{false};
        icon_overlay_mode icon_overlay{icon_overlay_mode::profile_on_browser};
        int highlight_width{4};
        bool rediscover_on_config_startup{true};
        bool discover_black_hole_browser{false};
        // Start the resident process at Windows sign-in, so the first click is fast too.
        bool run_at_startup{true};
        bool show_tray_icon{true};
        // Append click-to-picker timings to timing.log in the config folder.
        bool log_timing{false};
        toast_state toast;
        picker_state picker;
        transforms_state transforms;
        pipevis_state pipevis;
        std::vector<browser> browsers;

        bool operator==(const state &) const = default;
    };

    void from_node(const fkyaml::node& node, state& state);

    void to_node(fkyaml::node &node, const state &state);

    /**
     * @brief Loads and saves the state to config.yml.
     *
     * The resident process owns the state. It reloads the file when another process (the
     * command line, or a hand edit) changed it, and saves after each change made in the UI.
     */
    class config_store {
    public:
        /**
         * @brief Loads config.yml. When the file does not parse, the state stays as it was and the store
         * refuses to save until a later load succeeds, so a typo never costs the user their rules.
         */
        void load(state& s);

        /**
         * @brief Writes config.yml through a temporary file, so readers never see half a file.
         * @return false when nothing was written because the file on disk is broken.
         */
        bool save(const state& s);

        /**
         * @brief Reloads when the file on disk is newer than what we last read or wrote.
         * @return true when the state was reloaded.
         */
        bool reload_if_changed(state& s);

        [[nodiscard]] std::string get_path() const;

        /**
         * @brief The parse error of the file on disk, empty when it loaded fine.
         */
        [[nodiscard]] const std::string& get_error() const { return error; }

    private:
        std::filesystem::file_time_type last_write_time{};
        std::string error;
    };
}
