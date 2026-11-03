#include "config_window.h"
#include <memory>
#include <format>
#include <filesystem>
#include <commctrl.h>
#include <commdlg.h>
#include "form.h"
#include "win32ui.h"
#include "icons.h"
#include "tools.h"
#include "settings_dialog.h"
#include "globals.h"
#include "../host.h"
#include "../setup.h"
#include "../strings.h"
#include "../url_opener.h"
#include "common/str.h"
#include "common/fss.h"
#include "common/stl.hpp"
#include "magic_enum/magic_enum.hpp"

using namespace std;
using namespace grey::common;

namespace bt::ui::config_window {

    namespace {
        enum : int {
            ID_TREE = 200, ID_ADD, ID_REDISCOVER, ID_UP, ID_DOWN, ID_SHOW_HIDDEN, ID_STATUS,
            ID_RULES = 220, ID_RULE_ADD, ID_RULE_EDIT, ID_RULE_DEL, ID_RULE_UP, ID_RULE_DOWN, ID_RULE_CLEAR, ID_RULE_HELP,
            ID_ICON = 240,
            // menu
            M_ADD_BROWSER = 300, M_OPEN_CONFIG_DIR, M_CLOSE, M_EXIT_APP,
            M_DEFAULT_APPS, M_PIPELINE, M_SCRIPT, M_SUBS, M_REDISCOVER, M_BLACK_HOLE, M_HEALTH,
            M_OPTIONS, M_PICKER_OPTIONS,
            M_DOCS, M_RELEASES, M_ABOUT
        };

        struct state_t {
            HWND wnd{nullptr};
            HWND tree{nullptr}, status{nullptr}, rules{nullptr}, icon{nullptr};
            HWND btn_add{}, btn_rediscover{}, btn_up{}, btn_down{}, chk_hidden{};
            vector<HWND> rule_buttons;
            HIMAGELIST images{nullptr};
            unique_ptr<form> f;
            UINT dpi{96};
            int sel_browser{0};
            int sel_profile{0};
            bool building_tree{false};
            vector<system_check> health;
        } s;

        void rebuild_detail();
        void rebuild_tree();

        void changed() {
            host::state_changed();
        }

        browser* selected_browser() {
            if(s.sel_browser < 0 || s.sel_browser >= static_cast<int>(g_state.browsers.size())) return nullptr;
            return &g_state.browsers[s.sel_browser];
        }

        browser_profile* selected_profile() {
            browser* b = selected_browser();
            if(!b || s.sel_profile < 0 || s.sel_profile >= static_cast<int>(b->profiles.size())) return nullptr;
            return &b->profiles[s.sel_profile];
        }

        // ---- status bar

        void refresh_status() {
            if(!s.status) return;
            s.health = setup::get_checks();
            string health_text;
            for(auto& c : s.health) {
                c.recheck();
                if(!c.is_ok) health_text = "⚠ " + c.name + ": not set. Click here to fix.";
            }
            if(health_text.empty()) health_text = "✓ Default browser";

            size_t profiles{0}, rules{0};
            for(auto& b : g_state.browsers) {
                profiles += b.profiles.size();
                for(auto& p : b.profiles) rules += p.rules.size();
            }
            string counts = format("{} browsers · {} profiles · {} rules", g_state.browsers.size(), profiles, rules);
            string def;
            if(auto sel = browser::get_default(g_state.browsers)) {
                def = "Default: " + sel->b().name;
                if(sel->b().engine != browser_engine::generic) def += " / " + sel->p().name;
            }

            const int w1 = scale(330, s.dpi), w2 = scale(250, s.dpi);
            int parts[] = {w1, w1 + w2, -1};
            ::SendMessageW(s.status, SB_SETPARTS, 3, reinterpret_cast<LPARAM>(parts));
            ::SendMessageW(s.status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(wide(health_text).c_str()));
            ::SendMessageW(s.status, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(wide(counts).c_str()));
            ::SendMessageW(s.status, SB_SETTEXTW, 2, reinterpret_cast<LPARAM>(wide(def).c_str()));
        }

        // ---- tree

        LPARAM tree_key(int b, int p) {
            return (static_cast<LPARAM>(b) << 16) | static_cast<LPARAM>(p + 1);
        }

        HTREEITEM tree_insert(HTREEITEM parent, const string& text, int image, LPARAM key) {
            wstring t = wide(text);
            TVINSERTSTRUCTW ins{};
            ins.hParent = parent;
            ins.hInsertAfter = TVI_LAST;
            ins.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
            ins.item.pszText = t.data();
            ins.item.lParam = key;
            ins.item.iImage = ins.item.iSelectedImage = image;
            return reinterpret_cast<HTREEITEM>(::SendMessageW(s.tree, TVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&ins)));
        }

        void rebuild_tree() {
            s.building_tree = true;
            ::SendMessageW(s.tree, WM_SETREDRAW, FALSE, 0);
            ::SendMessageW(s.tree, TVM_DELETEITEM, 0, reinterpret_cast<LPARAM>(TVI_ROOT));

            const int px = scale(20, s.dpi);
            if(s.images) ImageList_Destroy(s.images);
            s.images = ImageList_Create(px, px, ILC_COLOR32, 16, 16);
            ::SendMessageW(s.tree, TVM_SETIMAGELIST, TVSIL_NORMAL, reinterpret_cast<LPARAM>(s.images));
            auto add_icon = [](HICON icon) {
                int i = ImageList_AddIcon(s.images, icon);
                ::DestroyIcon(icon);
                return i;
            };

            HTREEITEM to_select{nullptr};
            for(int bi = 0; bi < static_cast<int>(g_state.browsers.size()); bi++) {
                const browser& b = g_state.browsers[bi];
                if(b.is_hidden && !g_state.show_hidden_browsers) continue;

                string label = b.name;
                if(b.is_hidden) label += " (hidden)";
                if(b.is_default()) label += "  ★";
                int img = add_icon(icons::path_icon(b.get_best_icon_path(), px));
                HTREEITEM bitem = tree_insert(TVI_ROOT, label, img, tree_key(bi, -1));
                if(bi == s.sel_browser && (b.management == management_extent::none || s.sel_profile < 0)) to_select = bitem;

                // a manually configured browser has one profile; it is edited on the browser node
                if(b.management == management_extent::none) continue;

                for(int pi = 0; pi < static_cast<int>(b.profiles.size()); pi++) {
                    const browser_profile& p = b.profiles[pi];
                    if(p.is_hidden && !g_state.show_hidden_browsers) continue;
                    string pl = p.name;
                    if(p.is_hidden) pl += " (hidden)";
                    if(p.is_default) pl += "  ★";
                    if(!p.rules.empty()) pl += format("  [{}]", p.rules.size());
                    int pimg = add_icon(icons::profile_icon(b, p, px, icon_overlay_mode::profile_only));
                    HTREEITEM pitem = tree_insert(bitem, pl, pimg, tree_key(bi, pi));
                    if(bi == s.sel_browser && pi == s.sel_profile) to_select = pitem;
                }
                ::SendMessageW(s.tree, TVM_EXPAND, TVE_EXPAND, reinterpret_cast<LPARAM>(bitem));
            }

            ::SendMessageW(s.tree, WM_SETREDRAW, TRUE, 0);
            s.building_tree = false;
            if(!to_select) to_select = reinterpret_cast<HTREEITEM>(::SendMessageW(s.tree, TVM_GETNEXTITEM, TVGN_ROOT, 0));
            if(to_select) ::SendMessageW(s.tree, TVM_SELECTITEM, TVGN_CARET, reinterpret_cast<LPARAM>(to_select));
            else rebuild_detail();
        }

        void on_tree_selection(LPARAM key) {
            s.sel_browser = static_cast<int>(key >> 16);
            int p = static_cast<int>(key & 0xFFFF) - 1;
            if(p < 0) {
                // browser node: edit its first profile below it
                p = 0;
                if(browser* b = selected_browser(); b && b->management != management_extent::none) p = -1;
            }
            s.sel_profile = p;
            rebuild_detail();
        }

        // ---- rules list

        void refresh_rules() {
            ::SendMessageW(s.rules, LVM_DELETEALLITEMS, 0, 0);
            browser_profile* p = selected_profile();
            for(HWND h : s.rule_buttons) ::EnableWindow(h, p != nullptr);
            ::EnableWindow(s.rules, p != nullptr);
            if(!p) return;
            static const char* where[] = {"URL", "Window title", "Process", "Lua"};
            static const char* scope[] = {"anywhere", "host", "path"};
            for(int i = 0; i < static_cast<int>(p->rules.size()); i++) {
                const match_rule& r = p->rules[i];
                vector<string> cells{
                    where[static_cast<int>(r.loc)],
                    r.value,
                    r.loc == match_location::url ? scope[static_cast<int>(r.scope)] : "",
                    r.is_regex ? "yes" : "",
                    r.app_mode ? "yes" : ""};
                for(int c = 0; c < static_cast<int>(cells.size()); c++) {
                    wstring t = wide(cells[c]);
                    LVITEMW it{};
                    it.mask = LVIF_TEXT;
                    it.iItem = i;
                    it.iSubItem = c;
                    it.pszText = t.data();
                    ::SendMessageW(s.rules, c == 0 ? LVM_INSERTITEMW : LVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&it));
                }
            }
        }

        int selected_rule() {
            return static_cast<int>(::SendMessageW(s.rules, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
        }

        void select_rule(int i) {
            LVITEMW it{};
            it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
            it.state = LVIS_SELECTED | LVIS_FOCUSED;
            ::SendMessageW(s.rules, LVM_SETITEMSTATE, i, reinterpret_cast<LPARAM>(&it));
        }

        void rules_changed(int select = -1) {
            changed();
            refresh_rules();
            if(select >= 0) select_rule(select);
            refresh_status();
        }

        void on_rule_command(int id) {
            browser* b = selected_browser();
            browser_profile* p = selected_profile();
            if(!b || !p) return;
            int i = selected_rule();
            const int n = static_cast<int>(p->rules.size());
            switch(id) {
                case ID_RULE_ADD: {
                    match_rule r;
                    if(edit_rule(s.wnd, *b, r)) {
                        p->rules.push_back(r);
                        rules_changed(static_cast<int>(p->rules.size()) - 1);
                    }
                    break;
                }
                case ID_RULE_EDIT:
                    if(i >= 0 && i < n && edit_rule(s.wnd, *b, p->rules[i])) rules_changed(i);
                    break;
                case ID_RULE_DEL:
                    if(i >= 0 && i < n) {
                        p->rules.erase(p->rules.begin() + i);
                        rules_changed(min(i, n - 2));
                    }
                    break;
                case ID_RULE_UP:
                    if(i > 0 && i < n) {
                        swap(p->rules[i], p->rules[i - 1]);
                        rules_changed(i - 1);
                    }
                    break;
                case ID_RULE_DOWN:
                    if(i >= 0 && i < n - 1) {
                        swap(p->rules[i], p->rules[i + 1]);
                        rules_changed(i + 1);
                    }
                    break;
                case ID_RULE_CLEAR:
                    if(n > 0 && ::MessageBoxW(s.wnd, L"Delete all rules of this profile?", L"Rules", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                        p->rules.clear();
                        rules_changed();
                    }
                    break;
                case ID_RULE_HELP:
                    shell_open(string{APP_HELP_BASE_URL} + "#rules");
                    break;
                default:
                    break;
            }
        }

        // ---- detail panel (right side)

        int left_width() { return scale(280, s.dpi); }

        void layout_rules(int top) {
            RECT rc;
            ::GetClientRect(s.wnd, &rc);
            RECT sb;
            ::GetWindowRect(s.status, &sb);
            const int status_h = sb.bottom - sb.top;
            const int m = scale(10, s.dpi), x = left_width() + m * 2, bh = scale(26, s.dpi);
            int bx = x;
            for(HWND h : s.rule_buttons) {
                int w = scale(h == s.rule_buttons.back() ? 60 : 76, s.dpi);
                ::MoveWindow(h, bx, top, w, bh, TRUE);
                bx += w + scale(4, s.dpi);
            }
            int ly = top + bh + scale(6, s.dpi);
            ::MoveWindow(s.rules, x, ly, rc.right - x - m, max<int>(scale(60, s.dpi), rc.bottom - status_h - ly - m), TRUE);
        }

        void pick_colour(browser_profile& p) {
            static COLORREF custom[16]{};
            CHOOSECOLORW cc{sizeof(cc)};
            cc.hwndOwner = s.wnd;
            cc.rgbResult = RGB(p.user_color.r8(), p.user_color.g8(), p.user_color.b8());
            cc.lpCustColors = custom;
            cc.Flags = CC_RGBINIT | CC_FULLOPEN;
            if(::ChooseColorW(&cc)) {
                p.user_color = grey::rgb_colour::from_bytes(GetRValue(cc.rgbResult), GetGValue(cc.rgbResult), GetBValue(cc.rgbResult));
                p.use_user_color = true;
                icons::clear();
                changed();
                rebuild_tree();
            }
        }

        void rebuild_detail() {
            RECT rc;
            ::GetClientRect(s.wnd, &rc);
            const int m = scale(10, s.dpi);
            const int x = left_width() + m * 2;
            const int w = rc.right - x - m;
            form& f = *s.f;
            ::SendMessageW(s.wnd, WM_SETREDRAW, FALSE, 0);
            f.begin(x, m, w, scale(110, s.dpi), s.dpi);
            ::ShowWindow(s.icon, SW_HIDE);

            browser* bp = selected_browser();
            if(!bp) {
                if(g_state.browsers.empty()) {
                    f.section("No browsers yet");
                    f.label("Scan the system for installed browsers, or add one by hand.", true);
                    f.button("Discover browsers", [] { host::rediscover(); });
                }
                ::ShowWindow(s.rules, SW_HIDE);
                for(HWND h : s.rule_buttons) ::ShowWindow(h, SW_HIDE);
                ::SendMessageW(s.wnd, WM_SETREDRAW, TRUE, 0);
                ::RedrawWindow(s.wnd, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN);
                return;
            }
            browser& b = *bp;
            const bool full = b.management == management_extent::full;

            // browser
            f.section(b.name);
            if(!full) f.text("Name", b.name, false, [] { changed(); });
            if(!b.is_internal()) {
                f.text("Command", b.open_cmd, full, changed, 70);
                f.button("Folder", [&b] { shell_open(filesystem::path{b.open_cmd}.parent_path().string()); }, true, scale(64, s.dpi));
                if(!b.data_path.empty()) {
                    f.text("Data folder", b.data_path, full, changed, 70);
                    f.button("Open", [&b] { shell_open(b.data_path); }, true, scale(64, s.dpi));
                }
            }
            string engine = b.engine == browser_engine::chromium ? strings::ChromiumBased
                          : b.engine == browser_engine::gecko ? strings::GeckoBased : "Generic";
            string managed = b.management == management_extent::full ? "found by discovery"
                           : b.management == management_extent::profiles ? "added by hand, profiles discovered" : "added by hand";
            f.label(format("{} · {} · {} profile(s)", engine, managed, b.profiles.size()), true);
            f.check("Show in picker", [&b] { return !b.is_hidden; }, [&b](bool v) {
                b.is_hidden = !v;
                changed();
                rebuild_tree();
            });
            if(b.engine == browser_engine::gecko) {
                f.check("Discover legacy profiles", b.discover_classic_gecko_profiles, changed);
                f.check("Discover containers", b.discover_containers, changed);
                f.button("Profile manager", [&b] { shell_open(b.open_cmd, "-P"); });
                f.button("about:profiles", [&b] { shell_open(b.open_cmd, "about:profiles"); }, true);
                f.button("Delete browser", [] {
                    if(::MessageBoxW(s.wnd, L"Delete this browser and all its rules?", L"Delete", MB_YESNO | MB_ICONWARNING) != IDYES) return;
                    g_state.browsers.erase(g_state.browsers.begin() + s.sel_browser);
                    s.sel_browser = max(0, s.sel_browser - 1);
                    s.sel_profile = 0;
                    changed();
                    rebuild_tree();
                    refresh_status();
                }, true);
            } else {
                f.button("Delete browser", [] {
                    if(::MessageBoxW(s.wnd, L"Delete this browser and all its rules?", L"Delete", MB_YESNO | MB_ICONWARNING) != IDYES) return;
                    g_state.browsers.erase(g_state.browsers.begin() + s.sel_browser);
                    s.sel_browser = max(0, s.sel_browser - 1);
                    s.sel_profile = 0;
                    changed();
                    rebuild_tree();
                    refresh_status();
                });
            }

            // profile
            browser_profile* pp = selected_profile();
            if(pp) {
                browser_profile& p = *pp;
                const int idx = s.sel_profile;
                f.section(b.management == management_extent::none ? "Launch" : format("Profile: {}{}", p.name, p.is_incognito ? " (private)" : ""));

                if(!b.is_internal()) {
                    const int icon_box = scale(48, s.dpi);
                    RECT ir = f.area(icon_box);
                    ::MoveWindow(s.icon, ir.left, ir.top, icon_box, icon_box, FALSE);
                    ::ShowWindow(s.icon, SW_SHOWNA);
                    ::InvalidateRect(s.icon, nullptr, TRUE);
                    f.button("Change icon...", [&p] {
                        string path = open_file_dialog(s.wnd, L"Profile icon", {{L"Images", L"*.png;*.ico;*.jpg;*.bmp;*.exe"}});
                        if(path.empty()) return;
                        p.user_icon_path = path;
                        icons::clear();
                        changed();
                        rebuild_tree();
                    });
                    f.button("Reset icon", [&p] {
                        p.user_icon_path.clear();
                        icons::clear();
                        changed();
                        rebuild_tree();
                    }, true, 0, !p.user_icon_path.empty());

                    if(b.management == management_extent::none) {
                        f.text("Arguments", p.launch_arg, false, changed);
                        f.check("Hide the program's window", p.no_window, changed);
                    } else {
                        f.text("Arguments", p.launch_arg, true);
                        f.text("Extra arguments", p.user_arg, false, changed);
                    }
                }

                if(b.management != management_extent::none) {
                    f.check("Show in picker", [&p] { return !p.is_hidden; }, [&p](bool v) {
                        p.is_hidden = !v;
                        changed();
                        rebuild_tree();
                    });
                }

                f.button(p.is_default ? "Default ★" : "Make default", [&p] {
                    browser::set_default(g_state.browsers, p);
                    changed();
                    rebuild_tree();
                    refresh_status();
                }, false, 0, !p.is_default);
                f.button("Test link", [&b, idx] { url_opener::open(profile_selection(b, idx), APP_TEST_URL); }, true);
                if(b.supports_frameless_windows()) {
                    f.button("Test as app", [&b, idx] {
                        click_payload up{APP_TEST_URL};
                        up.app_mode = true;
                        url_opener::open(profile_selection(b, idx), up);
                    }, true);
                }
                if(b.management != management_extent::none) {
                    f.button("◀", [&b, idx] {
                        if(idx <= 0) return;
                        swap(b.profiles[idx], b.profiles[idx - 1]);
                        s.sel_profile = idx - 1;
                        changed();
                        rebuild_tree();
                    }, true, scale(32, s.dpi), idx > 0);
                    f.button("▶", [&b, idx] {
                        if(idx + 1 >= static_cast<int>(b.profiles.size())) return;
                        swap(b.profiles[idx], b.profiles[idx + 1]);
                        s.sel_profile = idx + 1;
                        changed();
                        rebuild_tree();
                    }, true, scale(32, s.dpi), idx + 1 < static_cast<int>(b.profiles.size()));
                }
                f.check("Custom highlight colour", p.use_user_color, [] {
                    icons::clear();
                    changed();
                    rebuild_tree();
                });
                if(p.use_user_color) {
                    f.button("Pick colour...", [&p] { pick_colour(p); }, true);
                }
            }

            f.section("Rules");
            ::ShowWindow(s.rules, SW_SHOWNA);
            for(HWND h : s.rule_buttons) ::ShowWindow(h, SW_SHOWNA);
            layout_rules(f.bottom());
            refresh_rules();

            ::SendMessageW(s.wnd, WM_SETREDRAW, TRUE, 0);
            ::RedrawWindow(s.wnd, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN);
        }

        void layout() {
            RECT rc;
            ::GetClientRect(s.wnd, &rc);
            ::SendMessageW(s.status, WM_SIZE, 0, 0);
            RECT sb;
            ::GetWindowRect(s.status, &sb);
            const int status_h = sb.bottom - sb.top;
            const int m = scale(10, s.dpi), bh = scale(26, s.dpi), lw = left_width();
            int x = m;
            for(HWND h : {s.btn_add, s.btn_rediscover, s.btn_up, s.btn_down}) {
                int w = h == s.btn_add ? scale(64, s.dpi) : h == s.btn_rediscover ? scale(84, s.dpi) : scale(32, s.dpi);
                ::MoveWindow(h, x, m, w, bh, TRUE);
                x += w + scale(4, s.dpi);
            }
            ::MoveWindow(s.chk_hidden, m, m + bh + scale(4, s.dpi), lw, scale(22, s.dpi), TRUE);
            int ty = m + bh + scale(30, s.dpi);
            ::MoveWindow(s.tree, m, ty, lw, rc.bottom - status_h - ty - m, TRUE);
            rebuild_detail();
        }

        HMENU build_menu() {
            HMENU bar = ::CreateMenu();
            HMENU file = ::CreatePopupMenu();
            ::AppendMenuW(file, MF_STRING, M_ADD_BROWSER, L"&Add custom browser...");
            ::AppendMenuW(file, MF_STRING, M_OPEN_CONFIG_DIR, L"&Open configuration folder");
            ::AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
            ::AppendMenuW(file, MF_STRING, M_CLOSE, L"&Close window");
            ::AppendMenuW(file, MF_STRING, M_EXIT_APP, L"E&xit Browser Tamer");
            HMENU tools = ::CreatePopupMenu();
            ::AppendMenuW(tools, MF_STRING, M_DEFAULT_APPS, L"Windows &default apps...");
            ::AppendMenuW(tools, MF_STRING, M_PIPELINE, wide(strings::PipelineDebugger + "...").c_str());
            ::AppendMenuW(tools, MF_STRING, M_SCRIPT, wide(strings::ScriptEditor + "...").c_str());
            ::AppendMenuW(tools, MF_STRING, M_SUBS, L"&Substitutions...");
            ::AppendMenuW(tools, MF_SEPARATOR, 0, nullptr);
            ::AppendMenuW(tools, MF_STRING, M_REDISCOVER, L"&Rediscover browsers");
            ::AppendMenuW(tools, MF_STRING | (g_state.discover_black_hole_browser ? MF_CHECKED : 0), M_BLACK_HOLE, L"Discover &Black Hole browser");
            ::AppendMenuW(tools, MF_STRING, M_HEALTH, L"Re-check &health");
            HMENU options = ::CreatePopupMenu();
            ::AppendMenuW(options, MF_STRING, M_OPTIONS, L"&Options...");
            ::AppendMenuW(options, MF_STRING, M_PICKER_OPTIONS, L"&Picker...");
            HMENU help = ::CreatePopupMenu();
            ::AppendMenuW(help, MF_STRING, M_DOCS, L"&Documentation");
            ::AppendMenuW(help, MF_STRING, M_RELEASES, L"All &releases");
            ::AppendMenuW(help, MF_SEPARATOR, 0, nullptr);
            ::AppendMenuW(help, MF_STRING, M_ABOUT, L"&About");
            ::AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
            ::AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(tools), L"&Tools");
            ::AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(options), L"&Settings");
            ::AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"&Help");
            return bar;
        }

        void move_selected(int delta) {
            browser* b = selected_browser();
            if(!b) return;
            const bool profile_node = b->management != management_extent::none && s.sel_profile >= 0;
            if(profile_node) {
                int to = s.sel_profile + delta;
                if(to < 0 || to >= static_cast<int>(b->profiles.size())) return;
                swap(b->profiles[s.sel_profile], b->profiles[to]);
                s.sel_profile = to;
            } else {
                int to = s.sel_browser + delta;
                if(to < 0 || to >= static_cast<int>(g_state.browsers.size())) return;
                swap(g_state.browsers[s.sel_browser], g_state.browsers[to]);
                s.sel_browser = to;
            }
            changed();
            rebuild_tree();
        }

        void on_command(int id, int code, LPARAM lp) {
            if(id >= ID_RULE_ADD && id <= ID_RULE_HELP) {
                on_rule_command(id);
                return;
            }
            switch(id) {
                case ID_ADD:
                case M_ADD_BROWSER:
                    if(add_browser(s.wnd)) {
                        s.sel_browser = static_cast<int>(g_state.browsers.size()) - 1;
                        s.sel_profile = 0;
                        rebuild_tree();
                        refresh_status();
                    }
                    return;
                case ID_REDISCOVER:
                case M_REDISCOVER:
                    host::rediscover();
                    return;
                case ID_UP:
                    move_selected(-1);
                    return;
                case ID_DOWN:
                    move_selected(1);
                    return;
                case ID_SHOW_HIDDEN:
                    g_state.show_hidden_browsers = ::SendMessageW(s.chk_hidden, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    changed();
                    rebuild_tree();
                    return;
                case M_OPEN_CONFIG_DIR:
                    shell_open(fss::get_config_dir(CONFIG_NAME));
                    return;
                case M_CLOSE:
                    ::DestroyWindow(s.wnd);
                    return;
                case M_EXIT_APP:
                    ::DestroyWindow(s.wnd);
                    host::quit();
                    return;
                case M_DEFAULT_APPS:
                    for(auto& c : s.health) c.fix();
                    if(s.health.empty()) shell_open("ms-settings:defaultapps");
                    return;
                case M_PIPELINE:
                    show_pipeline_debugger();
                    return;
                case M_SCRIPT:
                    show_script_editor();
                    return;
                case M_SUBS:
                    show_substitutions(s.wnd);
                    return;
                case M_BLACK_HOLE: {
                    g_state.discover_black_hole_browser = !g_state.discover_black_hole_browser;
                    ::CheckMenuItem(::GetMenu(s.wnd), M_BLACK_HOLE, g_state.discover_black_hole_browser ? MF_CHECKED : MF_UNCHECKED);
                    host::rediscover();
                    return;
                }
                case M_HEALTH:
                    refresh_status();
                    return;
                case M_OPTIONS:
                    show_settings_dialog(s.wnd, 0);
                    return;
                case M_PICKER_OPTIONS:
                    show_settings_dialog(s.wnd, 1);
                    return;
                case M_DOCS:
                    shell_open(APP_DOCS_URL);
                    return;
                case M_RELEASES:
                    shell_open(APP_GITHUB_RELEASES_URL);
                    return;
                case M_ABOUT:
                    message_box(s.wnd, "About",
                        format("{} {} (Win32)\n\nRuns in the background and shows the picker when a link needs one.\n\n"
                               "Click to picker, most recent first:\n{}", APP_LONG_NAME, APP_VERSION, host::timing_summary()));
                    return;
                default:
                    if(s.f) s.f->on_command(MAKEWPARAM(id, code), lp);
                    return;
            }
        }

        optional<LRESULT> handle(HWND hwnd, UINT m, WPARAM w, LPARAM l) {
            switch(m) {
                case WM_SIZE:
                    if(s.tree) layout();
                    return 0;
                case WM_GETMINMAXINFO: {
                    auto* mmi = reinterpret_cast<MINMAXINFO*>(l);
                    mmi->ptMinTrackSize = {scale(860, s.dpi), scale(640, s.dpi)};
                    return 0;
                }
                case WM_DPICHANGED: {
                    s.dpi = HIWORD(w);
                    auto* r = reinterpret_cast<RECT*>(l);
                    ::SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
                    rebuild_tree();
                    return 0;
                }
                case WM_ACTIVATE:
                    if(LOWORD(w) != WA_INACTIVE) refresh_status();
                    return nullopt;
                case WM_COMMAND:
                    on_command(LOWORD(w), HIWORD(w), l);
                    return 0;
                case WM_NOTIFY: {
                    auto* hdr = reinterpret_cast<NMHDR*>(l);
                    if(hdr->hwndFrom == s.tree && hdr->code == TVN_SELCHANGEDW && !s.building_tree) {
                        on_tree_selection(reinterpret_cast<NMTREEVIEWW*>(l)->itemNew.lParam);
                        return 0;
                    }
                    if(hdr->hwndFrom == s.rules && hdr->code == NM_DBLCLK) {
                        on_rule_command(selected_rule() >= 0 ? ID_RULE_EDIT : ID_RULE_ADD);
                        return 0;
                    }
                    if(hdr->hwndFrom == s.rules && hdr->code == LVN_KEYDOWN) {
                        auto* kd = reinterpret_cast<NMLVKEYDOWN*>(l);
                        if(kd->wVKey == VK_DELETE) on_rule_command(ID_RULE_DEL);
                        if(kd->wVKey == VK_RETURN) on_rule_command(ID_RULE_EDIT);
                        return 0;
                    }
                    if(hdr->hwndFrom == s.status && hdr->code == NM_CLICK) {
                        auto* mouse = reinterpret_cast<NMMOUSE*>(l);
                        if(mouse->dwItemSpec == 0) {
                            for(auto& c : s.health) if(!c.is_ok) c.fix();
                            refresh_status();
                        }
                        return 0;
                    }
                    break;
                }
                case WM_DRAWITEM: {
                    auto* di = reinterpret_cast<DRAWITEMSTRUCT*>(l);
                    if(di->CtlID == ID_ICON) {
                        ::FillRect(di->hDC, &di->rcItem, ::GetSysColorBrush(COLOR_WINDOW));
                        browser* b = selected_browser();
                        browser_profile* p = selected_profile();
                        if(b && p) {
                            HBITMAP bmp = icons::profile_bitmap(*b, *p, di->rcItem.right - di->rcItem.left, icon_overlay_mode::profile_only);
                            icons::draw(di->hDC, bmp, di->rcItem.left, di->rcItem.top);
                        }
                        return TRUE;
                    }
                    break;
                }
                case WM_CONTEXTMENU:
                    if(reinterpret_cast<HWND>(w) == s.icon) {
                        if(browser_profile* p = selected_profile(); p && !p->user_icon_path.empty()) {
                            p->user_icon_path.clear();
                            icons::clear();
                            changed();
                            rebuild_tree();
                        }
                        return 0;
                    }
                    break;
                case WM_DESTROY:
                    unregister_dialog(hwnd);
                    if(s.f) s.f->clear();
                    if(s.images) ImageList_Destroy(s.images);
                    s = state_t{};
                    return 0;
                default:
                    break;
            }
            return nullopt;
        }
    }

    HWND hwnd() {
        return s.wnd;
    }

    void state_reloaded() {
        if(!s.wnd) return;
        if(s.sel_browser >= static_cast<int>(g_state.browsers.size())) s.sel_browser = 0;
        rebuild_tree();
        refresh_status();
    }

    void show() {
        if(s.wnd) {
            if(::IsIconic(s.wnd)) ::ShowWindow(s.wnd, SW_RESTORE);
            ::SetForegroundWindow(s.wnd);
            return;
        }

        if(g_state.rediscover_on_config_startup) host::rediscover();

        UINT dpi{96};
        RECT work = cursor_work_area(&dpi);
        s.dpi = dpi;
        const int w = scale(1000, dpi), h = scale(720, dpi);
        s.wnd = create_window(0, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, format("{} {}", APP_LONG_NAME, APP_VERSION), nullptr,
            work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2, w, h,
            handle, build_menu());
        s.f = make_unique<form>(s.wnd);

        auto button = [](const string& text, int id) {
            return child(s.wnd, L"Button", text, BS_PUSHBUTTON | WS_TABSTOP, 0, id, s.dpi);
        };
        s.btn_add = button("Add...", ID_ADD);
        s.btn_rediscover = button("Rediscover", ID_REDISCOVER);
        s.btn_up = button("▲", ID_UP);
        s.btn_down = button("▼", ID_DOWN);
        s.chk_hidden = child(s.wnd, L"Button", "Show hidden browsers and profiles", BS_AUTOCHECKBOX | WS_TABSTOP, 0, ID_SHOW_HIDDEN, s.dpi);
        ::SendMessageW(s.chk_hidden, BM_SETCHECK, g_state.show_hidden_browsers ? BST_CHECKED : BST_UNCHECKED, 0);
        s.tree = child(s.wnd, WC_TREEVIEWW, "", TVS_SHOWSELALWAYS | TVS_HASBUTTONS | TVS_LINESATROOT | TVS_FULLROWSELECT | WS_TABSTOP,
            WS_EX_CLIENTEDGE, ID_TREE, s.dpi);
        ::SendMessageW(s.tree, TVM_SETEXTENDEDSTYLE, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
        ::SendMessageW(s.tree, TVM_SETITEMHEIGHT, scale(26, s.dpi), 0);
        s.status = ::CreateWindowExW(0, STATUSCLASSNAMEW, nullptr, WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0,
            s.wnd, reinterpret_cast<HMENU>(ID_STATUS), ::GetModuleHandleW(nullptr), nullptr);
        ::SendMessageW(s.status, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font(s.dpi)), FALSE);
        s.icon = child(s.wnd, L"Static", "", SS_OWNERDRAW | SS_NOTIFY, 0, ID_ICON, s.dpi);

        s.rules = child(s.wnd, WC_LISTVIEWW, "", LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL | WS_TABSTOP, WS_EX_CLIENTEDGE, ID_RULES, s.dpi);
        ::SendMessageW(s.rules, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        const pair<const wchar_t*, int> cols[] = {{L"Match", 90}, {L"Value", 300}, {L"Look in", 80}, {L"Regex", 60}, {L"App", 50}};
        for(int i = 0; i < 5; i++) {
            LVCOLUMNW c{};
            c.mask = LVCF_TEXT | LVCF_WIDTH;
            c.pszText = const_cast<wchar_t*>(cols[i].first);
            c.cx = scale(cols[i].second, s.dpi);
            ::SendMessageW(s.rules, LVM_INSERTCOLUMNW, i, reinterpret_cast<LPARAM>(&c));
        }
        for(auto [id, text] : {pair{ID_RULE_ADD, "Add..."}, pair{ID_RULE_EDIT, "Edit..."}, pair{ID_RULE_DEL, "Delete"},
                               pair{ID_RULE_UP, "Up"}, pair{ID_RULE_DOWN, "Down"}, pair{ID_RULE_CLEAR, "Clear all"}, pair{ID_RULE_HELP, "Help"}}) {
            s.rule_buttons.push_back(button(text, id));
        }

        rebuild_tree();
        layout();
        refresh_status();
        register_dialog(s.wnd);
        ::ShowWindow(s.wnd, SW_SHOW);
        ::SetForegroundWindow(s.wnd);
    }
}
