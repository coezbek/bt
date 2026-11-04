#pragma once
#include <windows.h>
#include <string>
#include "request.h"

/**
 * @brief The resident process: one hidden window, a tray icon, and the picker, toast and settings windows.
 *
 * bt.exe starts it once (at sign-in with --background, or on the first click). Later clicks arrive
 * as WM_COPYDATA from short-lived bt.exe processes. Config, Lua, ClearURLs and icons stay loaded,
 * which is what makes the picker fast.
 */
namespace bt::host {

    /**
     * @brief Becomes the resident process, handles the first request, then runs the message loop.
     */
    int run(const request& first);

    /**
     * @brief The UI changed g_state. Saves soon, and reloads the URL pipeline when asked.
     */
    void state_changed(bool reload_pipeline = false);

    /**
     * @brief Discovers browsers, merges them into the config and saves.
     */
    void rediscover();

    void show_config();

    /**
     * @brief Opens the settings window at a section, e.g. "picker" or "browsers".
     */
    void show_settings(const std::string& section);

    void quit();

    HWND window();

    /**
     * @brief Recent click-to-picker times, for the About box.
     */
    std::string timing_summary();
}
