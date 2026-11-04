#pragma once
#include <windows.h>
#include <string>

/**
 * @brief The settings window: navigation on the left, one long scrolling page of cards on the right.
 *
 * The page holds every setting: status, browsers with their profiles and rules, picker, toast,
 * link cleanup, general options, tools and about. Text, cards and icons are painted; only
 * interactive controls are child windows. The page is rebuilt after structural changes.
 */
namespace bt::ui::config_window {

    /**
     * @brief Shows the window, creating it on first use, and scrolls to a section if given
     * ("status", "browsers", "picker", "toast", "cleanup", "general", "tools", "about").
     */
    void show(const std::string& section = "");

    HWND hwnd();

    /**
     * @brief g_state was replaced (reload or rediscovery); rebuild the page.
     */
    void state_reloaded();
}
