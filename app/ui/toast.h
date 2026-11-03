#pragma once
#include <windows.h>
#include <string>
#include <optional>
#include "../browser.h"
#include "../click_payload.h"

namespace bt::ui {

    /**
     * @brief Small notice after a rule opened a link: where the click came from and which profile got it.
     *
     * It never takes focus, fades in and out, and stays while the mouse is over it.
     */
    class toast {
    public:
        void create();
        void show(const click_payload& cp, const browser& b, const browser_profile& p, const match_rule& rule);

    private:
        HWND wnd{nullptr};
        std::string line1, line2, rule_text, source_path;
        // copies, so a config reload while the toast shows cannot leave dangling references
        std::optional<browser> b;
        std::optional<browser_profile> p;
        int trackers{0};
        UINT dpi{96};
        bool dark{false};
        bool hovered{false};
        DWORD shown_at{0};
        enum class stage { hidden, fade_in, visible, fade_out } st{stage::hidden};
        DWORD stage_started{0};

        static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l);
        LRESULT handle(UINT m, WPARAM w, LPARAM l);
        void paint(HDC hdc);
        void tick();
        void set_alpha(float a);
    };
}
