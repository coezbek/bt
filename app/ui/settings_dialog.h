#pragma once
#include <windows.h>

namespace bt::ui {
    /**
     * @brief Modal dialog with general, picker, toast and transform options. Changes apply at once.
     * @param page 0 general, 1 picker, 2 toast, 3 transforms
     */
    void show_settings_dialog(HWND owner, int page);
}
