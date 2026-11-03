#pragma once
#include <windows.h>

namespace bt::ui::config_window {
    /**
     * @brief Shows the settings window (browsers, profiles, rules), creating it on first use.
     */
    void show();

    HWND hwnd();

    /**
     * @brief g_state was replaced (reload or rediscovery); rebuild the lists.
     */
    void state_reloaded();
}
