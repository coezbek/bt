#pragma once
#include "grey.h"
#include "model.h"
#include "../browser.h"
#include <vector>

/**
 * @brief Extra Grey widgets, specific to Browser Tamer
 */
namespace bt::ui {
    void btw_on_app_initialised(grey::app& app);

    /**
     * @brief Preloads only the icons needed to show the given choices.
     */
    void btw_preload_icons(grey::app& app, const std::vector<profile_selection>& choices);

    void btw_icon(grey::app& app,
        const profile_selection& selection,
        float padding_x, float padding_y,
        float icon_size);

    void btw_icon(grey::app& app,
        const browser& b, const browser_profile& p,
        icon_overlay_mode icon_mode,
        float padding_x, float padding_y,
        float icon_size);

    /**
     * @brief Render a rule widget
     *
     * @param b Browser
     * @param bi Browser profile
     * @param rule Rule to render
     * @param i Index of rule in browser profile, used for reordering logic. Pass -1 if not applicable.
     */
    void btw_rule(browser& b,
        browser_profile& bi,
        match_rule& rule,
        int i = -1);
}