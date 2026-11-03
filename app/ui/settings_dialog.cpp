#include "settings_dialog.h"
#include <memory>
#include <commctrl.h>
#include "form.h"
#include "win32ui.h"
#include "tools.h"
#include "globals.h"
#include "../host.h"

using namespace std;

namespace bt::ui {

    namespace {
        struct settings_ctx {
            HWND wnd{nullptr};
            HWND tabs{nullptr};
            HWND close_btn{nullptr};
            unique_ptr<form> f;
            int page{0};
            UINT dpi{96};
        };

        void changed() {
            host::state_changed();
        }

        void changed_pipeline() {
            host::state_changed(true);
        }

        void build_page(settings_ctx& c) {
            RECT rc;
            ::GetClientRect(c.wnd, &rc);
            ::SendMessageW(c.tabs, TCM_SETCURSEL, c.page, 0);

            const UINT d = c.dpi;
            form& f = *c.f;
            ::SendMessageW(c.wnd, WM_SETREDRAW, FALSE, 0);
            f.begin(scale(24, d), scale(48, d), rc.right - scale(48, d), scale(170, d), d);

            switch(c.page) {
                case 0: {
                    f.check("Run at Windows sign-in (makes the first click fast)", g_state.run_at_startup, changed);
                    f.check("Show tray icon", g_state.show_tray_icon, changed);
                    f.check("Rediscover browsers when settings open", g_state.rediscover_on_config_startup, changed);
                    f.check("Log clicks to clicks_<month>.csv", g_state.log_rule_hits, changed);
                    f.check("Log click-to-picker time to timing.log", g_state.log_timing, changed);
                    f.spacer(6);
                    f.combo("Icon drawing", {"Profile on top of browser", "Browser on top of profile", "Browser only", "Profile only"},
                        [] { return static_cast<int>(g_state.icon_overlay); },
                        [](int i) { g_state.icon_overlay = static_cast<icon_overlay_mode>(i); changed(); });
                    f.number("Colour ring width", g_state.highlight_width, 0, 20, changed);
                    break;
                }
                case 1: {
                    auto& p = g_state.picker;
                    f.check("Close when it loses focus", p.close_on_focus_loss, changed);
                    f.check("Always on top", p.always_on_top, changed);
                    f.check("Show when no rule matches", p.invoke.on_no_rule, changed);
                    f.check("Show key hints (1-0)", p.show_key_hints, changed);
                    f.check("Native window frame", p.show_native_chrome, changed);
                    f.section("Show the picker when these keys are down on click");
                    f.check("Ctrl + Shift", p.invoke.on_key_control_shift, changed);
                    f.check("Ctrl + Alt", p.invoke.on_key_control_alt, changed);
                    f.check("Alt + Shift", p.invoke.on_key_alt_shift, changed);
                    f.check("Caps Lock on", p.invoke.on_key_caps_locks, changed);
                    f.section("Look");
                    f.combo("Labels", {"None", "Browser and profile", "Browser", "Profile"},
                        [&p] { return static_cast<int>(p.label_display); },
                        [&p](int i) { p.label_display = static_cast<label_display_mode>(i); changed(); });
                    f.number("Item size", p.box_size, 16.0f, 256.0f, changed);
                    f.number("Item padding", p.item_padding, 0.0f, 100.0f, changed);
                    f.number("Item rounding", p.item_rounding, 0.0f, 128.0f, changed);
                    f.number("Label size", p.label_size, -15.0f, 15.0f, changed);
                    f.number("Min width (% of screen)", p.min_width_perc, 10, 90, changed);
                    f.number("Max width (% of screen)", p.max_width_perc, 20, 100, changed);
                    f.number("Border width", p.border_width, 0, 10, changed);
                    f.number("Opacity", p.opacity, 0.1f, 1.0f, changed);
                    f.spacer(6);
                    f.button("Reset picker settings", [&c] {
                        g_state.picker = picker_state{};
                        changed();
                        build_page(c);
                    });
                    break;
                }
                case 2: {
                    auto& t = g_state.toast;
                    f.check("Show a toast when a rule opens a link", t.enabled, changed);
                    f.number("Visible for (seconds)", t.visible_seconds, 0, 60, changed);
                    f.number("Border width", t.border_width, 0, 6, changed);
                    f.number("Opacity", t.opacity, 0.2f, 1.0f, changed);
                    f.number("Fade duration (seconds)", t.anim_duration, 0.0f, 5.0f, changed);
                    break;
                }
                case 3: {
                    auto& t = g_state.transforms;
                    f.check("Unwrap Office 365 safe links", t.unwrap_o365, changed_pipeline);
                    f.check("Unshorten short links", t.unshorten, changed_pipeline);
                    f.check("Remove trackers (ClearURLs)", t.clearurls, changed_pipeline);
                    f.check("Substitute text in links", t.substitute, changed_pipeline);
                    f.button("Edit substitutions...", [&c] { show_substitutions(c.wnd); });
                    f.spacer(4);
                    f.check("Run Lua pipeline functions (ppl_*)", t.scripting, changed_pipeline);
                    f.button("Open script editor...", [] { show_script_editor(); });
                    break;
                }
                default:
                    break;
            }

            // keep the tab control below the page controls so it does not paint over them
            ::SetWindowPos(c.tabs, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            ::SendMessageW(c.wnd, WM_SETREDRAW, TRUE, 0);
            ::RedrawWindow(c.wnd, nullptr, nullptr, RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN);
        }
    }

    void show_settings_dialog(HWND owner, int page) {
        static HWND open_dialog{nullptr};
        if(open_dialog && ::IsWindow(open_dialog)) {
            ::SetForegroundWindow(open_dialog);
            return;
        }

        auto ctx = make_shared<settings_ctx>();
        ctx->page = page;
        UINT dpi{96};
        RECT work = cursor_work_area(&dpi);
        ctx->dpi = dpi;
        const int w = scale(560, dpi), h = scale(800, dpi);

        HWND wnd = create_window(WS_EX_DLGMODALFRAME, WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
            APP_LONG_NAME " - Options", owner,
            work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2, w, h,
            [ctx](HWND hwnd, UINT m, WPARAM wp, LPARAM lp) -> optional<LRESULT> {
                switch(m) {
                    case WM_COMMAND:
                        if(LOWORD(wp) == IDCANCEL || LOWORD(wp) == IDOK) {
                            ::DestroyWindow(hwnd);
                            return 0;
                        }
                        if(ctx->f && ctx->f->on_command(wp, lp)) return 0;
                        break;
                    case WM_NOTIFY: {
                        auto* hdr = reinterpret_cast<NMHDR*>(lp);
                        if(hdr->hwndFrom == ctx->tabs && hdr->code == TCN_SELCHANGE) {
                            ctx->page = static_cast<int>(::SendMessageW(ctx->tabs, TCM_GETCURSEL, 0, 0));
                            build_page(*ctx);
                            return 0;
                        }
                        break;
                    }
                    case WM_DESTROY:
                        unregister_dialog(hwnd);
                        if(ctx->f) ctx->f->clear();
                        break;
                    default:
                        break;
                }
                return nullopt;
            });
        open_dialog = wnd;
        ctx->wnd = wnd;
        ctx->f = make_unique<form>(wnd);

        RECT rc;
        ::GetClientRect(wnd, &rc);
        ctx->tabs = child(wnd, WC_TABCONTROLW, "", WS_CLIPSIBLINGS | WS_TABSTOP, 0, 50, dpi);
        // the tab control is only the strip of tabs; pages sit on the plain window background below it
        ::MoveWindow(ctx->tabs, scale(8, dpi), scale(8, dpi), rc.right - scale(16, dpi), scale(28, dpi), FALSE);
        const wchar_t* names[] = {L"General", L"Picker", L"Toast", L"Transforms"};
        for(int i = 0; i < 4; i++) {
            TCITEMW ti{};
            ti.mask = TCIF_TEXT;
            ti.pszText = const_cast<wchar_t*>(names[i]);
            ::SendMessageW(ctx->tabs, TCM_INSERTITEMW, i, reinterpret_cast<LPARAM>(&ti));
        }
        ctx->close_btn = child(wnd, L"Button", "Close", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, IDOK, dpi);
        ::MoveWindow(ctx->close_btn, rc.right - scale(104, dpi), rc.bottom - scale(40, dpi), scale(96, dpi), scale(28, dpi), FALSE);

        build_page(*ctx);
        register_dialog(wnd);
        ::ShowWindow(wnd, SW_SHOW);
        ::SetForegroundWindow(wnd);
        run_modal(wnd, owner);
        open_dialog = nullptr;
    }
}
