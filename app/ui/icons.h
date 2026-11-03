#pragma once
#include <windows.h>
#include <string>
#include "../browser.h"
#include "model.h"

namespace bt::ui::icons {

    void init();
    void shutdown();

    /**
     * @brief Drops cached images, e.g. after a rediscovery or an icon change.
     */
    void clear();

    /**
     * @brief The icon a profile shows in the picker: browser and profile icons combined by mode,
     * clipped to a circle, with the profile colour as a ring. Premultiplied 32-bit DIB, cached; do not free.
     */
    HBITMAP profile_bitmap(const browser& b, const browser_profile& p, int px, icon_overlay_mode mode);

    /**
     * @brief A single image (file path, exe path or "res:..." key) as premultiplied DIB, cached; do not free.
     */
    HBITMAP path_bitmap(const std::string& path, int px, bool circle);

    /**
     * @brief Same as profile_bitmap, as an HICON for image lists. Caller destroys it.
     */
    HICON profile_icon(const browser& b, const browser_profile& p, int px, icon_overlay_mode mode);
    HICON path_icon(const std::string& path, int px);

    /**
     * @brief Draws a cached premultiplied bitmap with per-pixel alpha.
     */
    void draw(HDC hdc, HBITMAP bmp, int x, int y);

    /**
     * @brief Loads every icon the picker can show at the given size, so the first picker is fast.
     */
    void warm(int px, icon_overlay_mode mode);
}
