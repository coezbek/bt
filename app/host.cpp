#include "host.h"
#include <deque>
#include <map>
#include <format>
#include <fstream>
#include <filesystem>
#include <shellapi.h>
#include <commctrl.h>
#include <objbase.h>
#include "globals.h"
#include "url_opener.h"
#include "rule_hit_log.h"
#include "discovery.h"
#include "common/str.h"
#include "common/fss.h"
#include "common/process.h"
#include "common/datetime.h"
#include "ui/win32ui.h"
#include "ui/icons.h"
#include "ui/picker.h"
#include "ui/toast.h"
#include "ui/config_window.h"
#include "trace.h"

using namespace std;
using namespace grey::common;

namespace bt::host {

    namespace {
        HWND wnd{nullptr};
        ui::picker picker;
        ui::toast toast;
        UINT taskbar_created{0};
        bool tray_added{false};
        bool save_pending{false};
        deque<pair<string, double>> timings;

        constexpr UINT_PTR SAVE_TIMER = 1;
        constexpr UINT TRAY_ID = 1;
        constexpr wchar_t RUN_KEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

        enum : UINT { MENU_SETTINGS = 1, MENU_REDISCOVER, MENU_EXIT };

        HICON app_icon(int px) {
            return static_cast<HICON>(::LoadImageW(::GetModuleHandleW(nullptr), L"IDI_ICON1", IMAGE_ICON, px, px, LR_DEFAULTCOLOR));
        }

        void sync_tray() {
            NOTIFYICONDATAW nid{sizeof(nid)};
            nid.hWnd = wnd;
            nid.uID = TRAY_ID;
            if(!g_state.show_tray_icon) {
                if(tray_added) ::Shell_NotifyIconW(NIM_DELETE, &nid);
                tray_added = false;
                return;
            }
            if(tray_added) return;
            nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
            nid.uCallbackMessage = ui::WM_APP_TRAY;
            nid.hIcon = app_icon(::GetSystemMetricsForDpi(SM_CXSMICON, ui::dpi_of(wnd)));
            wcscpy_s(nid.szTip, ui::wide(APP_LONG_NAME).c_str());
            tray_added = ::Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
            nid.uVersion = NOTIFYICON_VERSION_4;
            ::Shell_NotifyIconW(NIM_SETVERSION, &nid);
        }

        void sync_startup() {
            if(!g_config.get_error().empty()) return;   // the setting is unknown while config.yml is broken
            HKEY key{nullptr};
            if(::RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return;
            // the fork used to register under the original name; drop that entry so it does not start twice
            ::RegDeleteValueW(key, ui::wide(APP_REGISTERED_NAME).c_str());
            const wstring name = ui::wide(APP_LONG_NAME);
            if(g_state.run_at_startup) {
                wchar_t exe[MAX_PATH]{};
                ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
                wstring cmd = format(L"\"{}\" --background", exe);
                ::RegSetValueExW(key, name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                    static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
            } else {
                ::RegDeleteValueW(key, name.c_str());
            }
            ::RegCloseKey(key);
        }

        /**
         * @brief Tells the user once per error that config.yml does not parse and is left alone.
         */
        void warn_if_config_broken() {
            static string warned;
            const string& e = g_config.get_error();
            if(e.empty() || e == warned) return;
            warned = e;
            ui::message_box(nullptr, APP_LONG_NAME,
                format("config.yml has an error and was not loaded:\n\n{}\n\n{} keeps working with the settings it had "
                       "and does not save until the file is fixed.\n\n{}", e, APP_LONG_NAME, g_config.get_path()),
                MB_OK | MB_ICONWARNING);
        }

        void save_now() {
            ::KillTimer(wnd, SAVE_TIMER);
            if(!save_pending) return;
            save_pending = false;
            g_config.save(g_state);
            sync_tray();
            sync_startup();
        }

        // Work that would replace or grow g_state.browsers (or a profile's rules) while a dialog holds
        // references into them; it runs when the last dialog closes.
        bool pending_rediscover{false};
        bool pending_reload{false};
        struct pending_rule {
            string browser_name, profile_name;
            match_rule rule;
        };
        vector<pending_rule> pending_rules;

        /**
         * @brief Picks up config.yml changes made outside this process (bt browser set default, hand edits).
         */
        void reload_if_changed() {
            if(ui::in_modal()) {
                pending_reload = true;
                return;
            }
            // scripts.lua may be edited in any editor; the old per-click process always read it fresh
            static filesystem::file_time_type script_time = [] {
                error_code ec;
                return filesystem::last_write_time(g_script.get_path(), ec);
            }();
            error_code ec;
            auto t = filesystem::last_write_time(g_script.get_path(), ec);
            if(!ec && t != script_time) {
                script_time = t;
                g_script.reload();
                g_pipeline.load();
            }

            if(save_pending) return;   // our own unsaved change wins
            if(g_config.reload_if_changed(g_state)) {
                g_pipeline.load();
                ui::config_window::state_reloaded();
            }
            warn_if_config_broken();
        }

        click_payload make_payload(const request& r) {
            click_payload cp{.url = r.url, .raw_url = r.url};
            cp.source_window_handle = reinterpret_cast<HWND>(r.source_hwnd);
            if(cp.source_window_handle && ::IsWindow(cp.source_window_handle)) {
                // InternalGetWindowText reads the cached title; GetWindowText would send WM_GETTEXT
                // and wait for the source app, which is often busy right after a click.
                wchar_t title[512]{};
                ::InternalGetWindowText(cp.source_window_handle, title, 512);
                cp.window_title = ui::narrow(title);
                DWORD pid{0};
                ::GetWindowThreadProcessId(cp.source_window_handle, &pid);
                cp.process_id = to_string(pid);
                process proc{static_cast<process::PidType>(pid)};
                cp.process_path = proc.get_path();
                cp.process_name = filesystem::path{cp.process_path}.filename().string();

                // the description comes from the exe's version resource; read each exe once
                static map<string, string> descriptions;
                auto it = descriptions.find(cp.process_path);
                if(it == descriptions.end()) it = descriptions.emplace(cp.process_path, proc.get_description()).first;
                cp.process_description = it->second;
            }
            cp.prettify();
            return cp;
        }

        void record_timing(const request& r, const string& what) {
            if(!r.launcher_start_100ns) return;
            FILETIME now{};
            ::GetSystemTimePreciseAsFileTime(&now);
            uint64_t now_100ns = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
            if(now_100ns < r.launcher_start_100ns) return;
            double ms = static_cast<double>(now_100ns - r.launcher_start_100ns) / 10000.0;
            timings.emplace_back(what, ms);
            while(timings.size() > 20) timings.pop_front();

            if(g_state.log_timing) {
                ofstream log{fss::get_config_file_path(CONFIG_NAME, "timing.log"), ios::app};
                log << datetime::to_iso_8601() << '\t' << what << '\t' << format("{:.1f}", ms) << '\n';
            }
        }

        void apply_rule(const string& browser_name, const string& profile_name, const match_rule& rule) {
            for(browser& b : g_state.browsers) {
                if(b.name != browser_name) continue;
                for(browser_profile& p : b.profiles) {
                    if(p.name != profile_name) continue;
                    if(ranges::find(p.rules, rule) == p.rules.end()) p.rules.push_back(rule);
                    state_changed();
                    ui::config_window::state_reloaded();
                    return;
                }
            }
        }

        void add_rule(const profile_selection& choice, const match_rule& rule) {
            if(ui::in_modal()) {
                pending_rules.push_back({choice.b().name, choice.p().name, rule});
                return;
            }
            apply_rule(choice.b().name, choice.p().name, rule);
        }

        void run_deferred() {
            auto rules = std::move(pending_rules);
            pending_rules.clear();
            for(auto& r : rules) apply_rule(r.browser_name, r.profile_name, r.rule);
            if(pending_reload) {
                pending_reload = false;
                reload_if_changed();
            }
            if(pending_rediscover) {
                pending_rediscover = false;
                rediscover();
            }
        }

        /**
         * @brief Routes one click: rules first, the picker when the user asked for it or rules disagree.
         */
        void open_url(const request& r, bool force_picker) {
            if(picker.visible()) picker.cancel();
            reload_if_changed();

            click_payload cp = make_payload(r);
            trace("payload ready");
            g_pipeline.process(cp);
            trace(format("pipeline done: {}", cp.url));

            bool show_picker{force_picker};
            optional<picker_invoked_reason> reason;
            vector<browser_match_result> matches;

            if(force_picker) {
                reason = picker_invoked_reason::forced;
            } else if(ui::picker::is_hotkey_down(r.modifiers)) {
                show_picker = true;
                reason = picker_invoked_reason::hotkey_down;
            } else {
                matches = browser::match(g_state.browsers, cp, g_script);
                if(matches.size() > 1) {
                    show_picker = true;
                    reason = picker_invoked_reason::rule_conflict;
                } else if(matches.size() == 1 && g_state.picker.invoke.on_no_rule && matches[0].rule.is_fallback) {
                    show_picker = true;
                    reason = picker_invoked_reason::no_rule_defined;
                }
            }

            if(show_picker) {
                vector<profile_selection> choices;
                if(reason == picker_invoked_reason::rule_conflict) {
                    for(auto& m : matches) choices.push_back(m.profile);
                } else {
                    for(const browser& b : g_state.browsers) {
                        if(b.is_hidden) continue;
                        for(size_t i = 0; i < b.profiles.size(); i++) {
                            if(!b.profiles[i].is_hidden) choices.emplace_back(b, i);
                        }
                    }
                }

                picker.show(cp, std::move(choices), [reason](optional<profile_selection> choice, const click_payload& final_cp, optional<match_rule> rule) {
                    if(!choice) return;
                    if(rule) add_rule(*choice, *rule);
                    click_payload up = final_cp;
                    if(rule) up.app_mode = rule->app_mode;
                    url_opener::open(*choice, up);
                    if(g_state.log_rule_hits) rule_hit_log::instance().write(up, *choice, reason, nullopt);
                });
                trace("picker shown");
                record_timing(r, "picker");
                return;
            }

            if(matches.empty()) matches = browser::match(g_state.browsers, cp, g_script);
            if(matches.empty()) {
                url_opener::open(cp);
                return;
            }

            browser_match_result& first = matches[0];
            first.rule.apply_to(cp);
            url_opener::open(first.profile, cp);
            record_timing(r, "direct");
            if(g_state.log_rule_hits) rule_hit_log::instance().write(cp, first.profile, nullopt, first);
            if(g_state.toast.enabled) toast.show(cp, first.profile.b(), first.profile.p(), first.rule);
        }

        void handle(const request& r) {
            if(r.launcher_start_100ns && r.launcher_main_100ns) {
                FILETIME ft{};
                ::GetSystemTimePreciseAsFileTime(&ft);
                uint64_t now = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
                trace(format("launcher: created->main {:.1f} ms, main->host {:.1f} ms",
                    (r.launcher_main_100ns - r.launcher_start_100ns) / 10000.0, (now - r.launcher_main_100ns) / 10000.0));
            }
            trace(format("request kind={} url={}", static_cast<int>(r.kind), r.url));
            switch(r.kind) {
                case request_kind::open:
                    open_url(r, false);
                    break;
                case request_kind::pick:
                    open_url(r, true);
                    break;
                case request_kind::config:
                    reload_if_changed();
                    show_config();
                    break;
                case request_kind::discover:
                    rediscover();
                    break;
                case request_kind::background:
                    break;
                case request_kind::exit:
                    quit();
                    break;
            }
        }

        void show_tray_menu() {
            HMENU menu = ::CreatePopupMenu();
            ::AppendMenuW(menu, MF_STRING, MENU_SETTINGS, L"&Settings");
            ::AppendMenuW(menu, MF_STRING, MENU_REDISCOVER, L"&Rediscover browsers");
            ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            ::AppendMenuW(menu, MF_STRING, MENU_EXIT, L"E&xit");
            ::SetMenuDefaultItem(menu, MENU_SETTINGS, FALSE);
            POINT pt;
            ::GetCursorPos(&pt);
            ::SetForegroundWindow(wnd);
            UINT cmd = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, wnd, nullptr);
            ::PostMessageW(wnd, WM_NULL, 0, 0);
            ::DestroyMenu(menu);
            switch(cmd) {
                case MENU_SETTINGS: show_config(); break;
                case MENU_REDISCOVER: rediscover(); break;
                case MENU_EXIT: quit(); break;
                default: break;
            }
        }

        LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
            if(m == taskbar_created && taskbar_created) {
                tray_added = false;
                sync_tray();
                return 0;
            }
            switch(m) {
                case WM_COPYDATA: {
                    auto* cds = reinterpret_cast<COPYDATASTRUCT*>(l);
                    if(!cds || cds->dwData != HOST_COPYDATA_ID) return FALSE;
                    auto r = request::deserialize(string{static_cast<const char*>(cds->lpData), cds->cbData});
                    if(!r) return FALSE;
                    // Release bt.exe now; COM calls are not allowed inside a sent message.
                    ::ReplyMessage(TRUE);
                    handle(*r);
                    return TRUE;
                }
                case ui::WM_APP_TRAY:
                    switch(LOWORD(l)) {
                        case WM_CONTEXTMENU:
                        case WM_RBUTTONUP:
                            show_tray_menu();
                            break;
                        case NIN_SELECT:
                        case NIN_KEYSELECT:
                        case WM_LBUTTONDBLCLK:
                            show_config();
                            break;
                        default:
                            break;
                    }
                    return 0;
                case ui::WM_APP_DEFERRED:
                    if(!ui::in_modal()) run_deferred();
                    return 0;
                case ui::WM_APP_WARM:
                    // decode icons now, so the first picker does not have to
                    ui::icons::warm(ui::picker::icon_size_for_dpi(::GetDpiForSystem()), g_state.icon_overlay);
                    picker.warm();
                    trace("icons and picker warm");
                    return 0;
                case WM_TIMER:
                    if(w == SAVE_TIMER) save_now();
                    return 0;
                case WM_SETTINGCHANGE:
                    // DPI or theme changes take effect on the next picker; nothing cached depends on them
                    return 0;
                case WM_QUERYENDSESSION:
                    save_now();
                    return TRUE;
                case WM_ENDSESSION:
                    // sign-out, or an installer asking us to close so it can replace bt.exe
                    if(w) {
                        save_now();
                        quit();
                    }
                    return 0;
                case WM_CLOSE:
                    quit();
                    return 0;
                case WM_DESTROY:
                    save_now();
                    if(tray_added) {
                        NOTIFYICONDATAW nid{sizeof(nid)};
                        nid.hWnd = h;
                        nid.uID = TRAY_ID;
                        ::Shell_NotifyIconW(NIM_DELETE, &nid);
                    }
                    ::PostQuitMessage(0);
                    return 0;
                default:
                    break;
            }
            return ::DefWindowProcW(h, m, w, l);
        }
    }

    void state_changed(bool reload_pipeline) {
        // the picker points into g_state.browsers; any edit may move what it points to
        if(picker.visible()) picker.cancel();
        if(reload_pipeline) g_pipeline.load();
        save_pending = true;
        ::SetTimer(wnd, SAVE_TIMER, 400, nullptr);
    }

    void rediscover() {
        if(ui::in_modal()) {
            pending_rediscover = true;
            return;
        }
        if(picker.visible()) picker.cancel();
        vector<browser> fresh = discovery::discover_all_browsers();
        // rediscover profiles of manually added browsers
        for(browser& b : g_state.browsers) {
            if(b.management != management_extent::profiles) continue;
            browser nb = b;
            nb.profiles.clear();
            discovery::discover_managed_profiles(nb);
            fresh.push_back(nb);
        }
        g_state.browsers = browser::merge(fresh, g_state.browsers);
        ui::icons::clear();
        state_changed();
        save_now();
        ui::config_window::state_reloaded();
    }

    void show_config() {
        if(picker.visible()) picker.cancel();
        ui::config_window::show();
    }

    void show_settings(const string& section) {
        if(picker.visible()) picker.cancel();
        ui::config_window::show(section);
    }

    void quit() {
        if(wnd) ::DestroyWindow(wnd);
    }

    HWND window() {
        return wnd;
    }

    string timing_summary() {
        if(timings.empty()) return "No clicks measured yet.";
        string s;
        for(auto it = timings.rbegin(); it != timings.rend(); ++it) {
            s += format("{:>6.1f} ms  {}\n", it->second, it->first);
        }
        return s;
    }

    int run(const request& first) {
        ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES |
                                              ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_UPDOWN_CLASS | ICC_LINK_CLASS};
        ::InitCommonControlsEx(&icc);
        trace("host start");
        ::SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* ep) -> LONG {
            trace(format("crash: code {:#x} at {}", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress));
            return EXCEPTION_CONTINUE_SEARCH;
        });
        ui::icons::init();
        ui::on_modal_end([] { ::PostMessageW(wnd, ui::WM_APP_DEFERRED, 0, 0); });
        bt_init_core();
        trace("core loaded");

        const HINSTANCE inst = ::GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = wndproc;
        wc.hInstance = inst;
        wc.lpszClassName = HOST_WINDOW_CLASS;
        ::RegisterClassExW(&wc);
        wnd = ::CreateWindowExW(WS_EX_TOOLWINDOW, HOST_WINDOW_CLASS, ui::wide(APP_LONG_NAME).c_str(), WS_POPUP,
            0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
        taskbar_created = ::RegisterWindowMessageW(L"TaskbarCreated");
        ::ChangeWindowMessageFilterEx(wnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
        if(taskbar_created) ::ChangeWindowMessageFilterEx(wnd, taskbar_created, MSGFLT_ALLOW, nullptr);

        picker.create();
        toast.create();
        trace("windows created");

        // first start ever: find the browsers before routing anything. Not when config.yml is merely broken.
        if(g_state.browsers.empty() && g_config.get_error().empty()) rediscover();

        sync_tray();
        sync_startup();
        warn_if_config_broken();

        handle(first);
        ::PostMessageW(wnd, ui::WM_APP_WARM, 0, 0);

        MSG msg;
        while(::GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if(ui::route_dialog_message(msg)) continue;
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }

        ui::icons::shutdown();
        ::CoUninitialize();
        return 0;
    }
}
