#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <functional>

namespace bt::ui {

    using msg_handler = std::function<std::optional<LRESULT>(HWND, UINT, WPARAM, LPARAM)>;

    /**
     * @brief Creates a window whose messages go to a handler. Unhandled messages get default behaviour:
     * white background for labels and check boxes, and WM_CLOSE destroys the window.
     */
    HWND create_window(DWORD ex_style, DWORD style, const std::string& title, HWND parent,
        int x, int y, int w, int h, msg_handler handler, HMENU menu = nullptr);

    /**
     * @brief Rows of labelled controls, bound straight to variables. Changes apply as the user types.
     *
     * The owner forwards WM_COMMAND to on_command(). clear() destroys the controls so the form can be
     * rebuilt for a new selection or size. Bound references must outlive the controls.
     */
    class form {
    public:
        explicit form(HWND parent) : parent{parent} {}

        void begin(int x, int y, int width, int label_width, UINT dpi);
        void clear();

        void section(const std::string& title);
        void label(const std::string& text, bool dim = false, int lines = 1);
        void check(const std::string& text, bool& value, std::function<void()> changed = {});
        void check(const std::string& text, std::function<bool()> get, std::function<void(bool)> set);
        /**
         * @brief A text box. reserve leaves room at the right for same_line buttons.
         */
        HWND text(const std::string& label, std::string& value, bool read_only = false, std::function<void()> changed = {}, int reserve = 0);
        void number(const std::string& label, int& value, int lo, int hi, std::function<void()> changed = {});
        void number(const std::string& label, float& value, float lo, float hi, std::function<void()> changed = {});
        HWND combo(const std::string& label, const std::vector<std::string>& items, std::function<int()> get,
            std::function<void(int)> set);

        /**
         * @brief A button. same_line puts it right of the previous control in the row.
         */
        HWND button(const std::string& text, std::function<void()> click, bool same_line = false, int width = 0, bool enabled = true);
        void spacer(int px);

        /**
         * @brief Reserves a full-width area and returns its rectangle, for custom controls.
         */
        RECT area(int height);

        [[nodiscard]] int bottom() const { return y; }
        [[nodiscard]] UINT get_dpi() const { return dpi; }
        bool on_command(WPARAM w, LPARAM l);

        /**
         * @brief Draws the rounded boxes behind text fields. Call from the parent's WM_PAINT.
         */
        void paint(HDC hdc) const;

    private:
        HWND parent;
        UINT dpi{96};
        int x0{0}, y{0}, width{0}, label_w{0};
        int row_h{0}, gap{0};
        int next_id{1000};
        int last_right{0}, last_top{0};
        bool updating{false};
        std::vector<HWND> controls;
        std::vector<std::pair<RECT, bool>> boxes;   // field box, read-only
        std::map<int, std::function<void(int code)>> handlers;

        int new_id() { return next_id++; }
        HWND make_label(const std::string& text, int x, int top, int w, int h, bool dim = false);
        void next_row(int h = 0);
    };
}
