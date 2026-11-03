#pragma once
#include <windows.h>
#include <vector>
#include <optional>
#include <functional>
#include "../browser.h"
#include "../click_payload.h"

namespace bt::ui {

    /**
     * @brief The browser picker. Created once, hidden, and reused for every click.
     *
     * show() lays out, paints into a back buffer and shows the window in one go. The window
     * reports the choice through a callback and hides itself again. Keyboard: arrows move,
     * Enter or 1-0 open, R toggles the rule creator, Esc closes.
     */
    class picker {
    public:
        using done_fn = std::function<void(std::optional<profile_selection> choice, const click_payload& cp, std::optional<match_rule> new_rule)>;

        void create();
        void show(const click_payload& cp, std::vector<profile_selection> choices, done_fn done);
        void cancel();

        /**
         * @brief Runs layout and rendering once while parked, so the first real click finds fonts,
         * styles, icons and GDI+ ready.
         */
        void warm();
        [[nodiscard]] bool visible() const;
        [[nodiscard]] HWND hwnd() const { return wnd; }

        /**
         * @brief Hotkey that forces the picker, as configured in settings.
         */
        static bool is_hotkey_down(uint32_t modifiers);

        /**
         * @brief Icon size in pixels the picker uses at a DPI, for warming the icon cache.
         */
        static int icon_size_for_dpi(UINT dpi);

    private:
        HWND wnd{nullptr};
        HWND url_edit{nullptr};
        HWND tooltip{nullptr};
        HWND btn_trackers{nullptr}, btn_rule{nullptr}, btn_copy{nullptr}, btn_email{nullptr}, btn_settings{nullptr};
        HWND rule_loc{nullptr}, rule_value{nullptr}, rule_scope{nullptr}, rule_regex{nullptr}, rule_app{nullptr};

        click_payload cp;
        std::vector<profile_selection> choices;
        done_fn done;
        int active{0};
        bool rule_open{false};
        bool rule_regex_on{false};
        bool rule_app_on{false};
        bool dark{false};
        bool closing{false};
        bool cloaked{false};
        int mouse_down{-1};     // tile under the last left button press

        // layout in pixels for the current DPI
        UINT dpi{96};
        int pad{0}, header_h{0}, rule_h{0}, btn_w{0};
        int box{0}, margin{0}, tile{0}, per_row{1}, rows{1}, grid_x{0}, grid_y{0};
        int icon_px{0}, label_lines{0}, label_h{0};
        SIZE client{};
        RECT url_box{}, rule_box{};   // field backgrounds drawn by us; the edits inside are one line high
        RECT target{};          // window rect for the current layout
        UINT font_dpi{0};       // DPI the child control fonts were set for

        // what apply_style() last set, so a show only touches what changed
        struct {
            LONG_PTR style{-1}, ex_style{-1};
            BYTE alpha{0};
            int dark{-1};
        } applied;

        // back buffer, rendered before the window shows and blitted on WM_PAINT
        HDC back_dc{nullptr};
        HBITMAP back_bmp{nullptr};
        HGDIOBJ back_old{nullptr};
        SIZE back_size{};

        static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l);
        static LRESULT CALLBACK edit_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref);
        LRESULT handle(UINT m, WPARAM w, LPARAM l);

        void create_controls();
        void set_cloaked(bool on);
        void park();
        bool apply_style();
        void compute_layout();
        void move_window(UINT extra_flags);
        void place_controls();
        void render();
        void paint(HDC hdc);
        void paint_tiles(HDC hdc);
        void draw_button(const DRAWITEMSTRUCT& di);
        int hit_test(POINT pt) const;
        RECT tile_rect(int i) const;
        void set_active(int i);
        void toggle_rule_creator();
        void choose(int i);
        void finish(std::optional<int> idx);
        void on_command(int id, int code);
        std::optional<match_rule> build_rule() const;
    };
}
