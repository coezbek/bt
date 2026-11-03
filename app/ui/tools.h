#pragma once
#include <windows.h>
#include <string>
#include "../browser.h"

namespace bt::ui {

    /**
     * @brief Modal rule editor. Returns true when the user pressed OK; rule is changed only then.
     */
    bool edit_rule(HWND owner, const browser& b, match_rule& rule);

    /**
     * @brief Modal "add custom browser" dialog. Returns true when a browser was added to g_state.
     */
    bool add_browser(HWND owner);

    /**
     * @brief Modal list of running process names. Returns the chosen name, or empty.
     */
    std::string pick_process_name(HWND owner);

    void show_substitutions(HWND owner);
    void show_script_editor();
    void show_pipeline_debugger();
}
