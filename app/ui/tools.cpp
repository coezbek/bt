#include "tools.h"
#include <memory>
#include <format>
#include <filesystem>
#include <algorithm>
#include <commctrl.h>
#include "form.h"
#include "win32ui.h"
#include "globals.h"
#include "../host.h"
#include "../discovery.h"
#include "../strings.h"
#include "../url_pipeline_step.h"
#include "../pipeline/replacer.h"
#include "common/str.h"
#include "common/process.h"
#include "common/win32/user.h"
#include "magic_enum/magic_enum.hpp"

using namespace std;
using namespace grey::common;

namespace bt::ui {

    namespace {
        // ---- list view helpers

        void lv_columns(HWND lv, const vector<pair<string, int>>& cols, UINT dpi) {
            for(int i = 0; i < static_cast<int>(cols.size()); i++) {
                wstring t = wide(cols[i].first);
                LVCOLUMNW c{};
                c.mask = LVCF_TEXT | LVCF_WIDTH;
                c.pszText = t.data();
                c.cx = scale(cols[i].second, dpi);
                ::SendMessageW(lv, LVM_INSERTCOLUMNW, i, reinterpret_cast<LPARAM>(&c));
            }
        }

        void lv_row(HWND lv, int row, const vector<string>& cells) {
            for(int i = 0; i < static_cast<int>(cells.size()); i++) {
                wstring t = wide(cells[i]);
                LVITEMW it{};
                it.mask = LVIF_TEXT;
                it.iItem = row;
                it.iSubItem = i;
                it.pszText = t.data();
                ::SendMessageW(lv, i == 0 ? LVM_INSERTITEMW : LVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&it));
            }
        }

        int lv_selected(HWND lv) {
            return static_cast<int>(::SendMessageW(lv, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
        }

        void lv_select(HWND lv, int row) {
            LVITEMW it{};
            it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
            it.state = LVIS_SELECTED | LVIS_FOCUSED;
            ::SendMessageW(lv, LVM_SETITEMSTATE, row, reinterpret_cast<LPARAM>(&it));
            ::SendMessageW(lv, LVM_ENSUREVISIBLE, row, FALSE);
        }

        HWND make_list_view(HWND parent, int id, UINT dpi) {
            HWND lv = child(parent, WC_LISTVIEWW, "", LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL | WS_TABSTOP,
                WS_EX_CLIENTEDGE, id, dpi);
            ::SendMessageW(lv, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            return lv;
        }

        POINT centred(HWND owner, int w, int h) {
            RECT r{};
            if(owner && ::IsWindowVisible(owner)) ::GetWindowRect(owner, &r);
            else r = cursor_work_area();
            return {r.left + (r.right - r.left - w) / 2, r.top + (r.bottom - r.top - h) / 2};
        }

        /**
         * @brief Modal dialog: a form plus OK and Cancel. build() fills the form and can rebuild it.
         * validate() runs on OK and keeps the dialog open when it returns false.
         */
        bool form_dialog(HWND owner, const string& title, int w_dip, int h_dip,
            function<void(form&, function<void()>)> build, function<bool()> validate = {}) {
            UINT dpi = owner ? dpi_of(owner) : ::GetDpiForSystem();
            const int w = scale(w_dip, dpi), h = scale(h_dip, dpi);
            POINT pos = centred(owner, w, h);

            struct ctx_t {
                unique_ptr<form> f;
                function<void()> rebuild;
                bool ok{false};
            };
            auto ctx = make_shared<ctx_t>();

            HWND wnd = create_window(WS_EX_DLGMODALFRAME, WS_POPUP | WS_CAPTION | WS_SYSMENU, title, owner,
                pos.x, pos.y, w, h,
                [ctx, validate, dpi](HWND hwnd, UINT m, WPARAM wp, LPARAM lp) -> optional<LRESULT> {
                    if(m == WM_PAINT) {
                        // white body with the fields, grey button bar at the bottom
                        PAINTSTRUCT ps;
                        HDC hdc = ::BeginPaint(hwnd, &ps);
                        RECT rc;
                        ::GetClientRect(hwnd, &rc);
                        ::FillRect(hdc, &rc, static_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH)));
                        static HBRUSH bar = ::CreateSolidBrush(RGB(243, 243, 243));
                        static HBRUSH line = ::CreateSolidBrush(RGB(229, 229, 229));
                        RECT b{0, rc.bottom - scale(56, dpi), rc.right, rc.bottom};
                        ::FillRect(hdc, &b, bar);
                        RECT l{0, b.top, rc.right, b.top + 1};
                        ::FillRect(hdc, &l, line);
                        if(ctx->f) ctx->f->paint(hdc);
                        ::EndPaint(hwnd, &ps);
                        return 0;
                    }
                    if(m == WM_CTLCOLORBTN) {
                        static HBRUSH bar = ::CreateSolidBrush(RGB(243, 243, 243));
                        return reinterpret_cast<LRESULT>(bar);
                    }
                    if(m == WM_COMMAND) {
                        if(LOWORD(wp) == IDOK) {
                            if(!validate || validate()) {
                                ctx->ok = true;
                                ::DestroyWindow(hwnd);
                            }
                            return 0;
                        }
                        if(LOWORD(wp) == IDCANCEL) {
                            ::DestroyWindow(hwnd);
                            return 0;
                        }
                        if(ctx->f && ctx->f->on_command(wp, lp)) return 0;
                    }
                    if(m == WM_DESTROY) {
                        unregister_dialog(hwnd);
                        if(ctx->f) ctx->f->clear();
                    }
                    return nullopt;
                });

            RECT rc;
            ::GetClientRect(wnd, &rc);
            ctx->f = make_unique<form>(wnd);
            ctx->rebuild = [ctx, wnd, dpi, rc, build] {
                ::SendMessageW(wnd, WM_SETREDRAW, FALSE, 0);
                ctx->f->begin(scale(16, dpi), scale(16, dpi), rc.right - scale(32, dpi), scale(110, dpi), dpi);
                build(*ctx->f, ctx->rebuild);
                ::SendMessageW(wnd, WM_SETREDRAW, TRUE, 0);
                ::RedrawWindow(wnd, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN);
            };
            ctx->rebuild();

            HWND ok = child(wnd, L"Button", "OK", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, IDOK, dpi);
            HWND cancel = child(wnd, L"Button", "Cancel", BS_PUSHBUTTON | WS_TABSTOP, 0, IDCANCEL, dpi);
            const int bw = scale(96, dpi), bh = scale(32, dpi), m = scale(12, dpi);
            ::MoveWindow(cancel, rc.right - m - bw, rc.bottom - m - bh, bw, bh, FALSE);
            ::MoveWindow(ok, rc.right - m * 2 - bw * 2 + scale(6, dpi), rc.bottom - m - bh, bw, bh, FALSE);

            register_dialog(wnd);
            ::ShowWindow(wnd, SW_SHOW);
            run_modal(wnd, owner);
            bool result = ctx->ok;
            ctx->rebuild = nullptr;   // break the self reference
            return result;
        }
    }

    // ---------------------------------------------------------------- rule editor

    bool edit_rule(HWND owner, const browser& b, match_rule& rule) {
        auto tmp = make_shared<match_rule>(rule);
        const bool frameless = b.supports_frameless_windows();
        bool ok = form_dialog(owner, "Rule", 540, 400, [tmp, frameless](form& f, function<void()> rebuild) {
            f.combo("Match", {"URL", "Window title", "Process name", strings::LuaScript},
                [tmp] { return static_cast<int>(tmp->loc); },
                [tmp, rebuild](int i) {
                    tmp->loc = static_cast<match_location>(i);
                    rebuild();
                });

            if(tmp->loc == match_location::lua_script) {
                const auto& names = g_script.rule_function_names;
                if(names.empty()) {
                    f.label("No rule_* functions in scripts.lua yet. Use Tools > Script editor.", true, 2);
                } else {
                    f.combo("Function", names,
                        [tmp, &names] {
                            auto it = ranges::find(names, tmp->value);
                            return it == names.end() ? 0 : static_cast<int>(it - names.begin());
                        },
                        [tmp, &names](int i) { tmp->value = names[i]; });
                    if(ranges::find(names, tmp->value) == names.end()) tmp->value = names[0];
                }
            } else {
                HWND edit = f.text("Contains", tmp->value, false, {}, tmp->loc == match_location::process_name ? 110 : 0);
                if(tmp->loc == match_location::process_name) {
                    f.button("Running...", [tmp, edit] {
                        string name = pick_process_name(::GetParent(edit));
                        if(!name.empty()) {
                            tmp->value = name;
                            set_text(edit, name);
                        }
                    }, true, 0);
                }
                if(tmp->loc == match_location::url) {
                    f.combo("Look in", {"Whole URL", "Host name only", "Path only"},
                        [tmp] { return static_cast<int>(tmp->scope); },
                        [tmp](int i) { tmp->scope = static_cast<match_scope>(i); });
                }
                f.check(strings::RuleIsARegex, tmp->is_regex);
            }
            if(frameless) f.check("Open as an app window (no tabs or address bar)", tmp->app_mode);
        }, [tmp, owner] {
            if(tmp->value.empty()) {
                message_box(owner, "Rule", "The rule needs a value.", MB_OK | MB_ICONWARNING);
                return false;
            }
            return true;
        });
        if(ok) rule = *tmp;
        return ok;
    }

    // ---------------------------------------------------------------- add browser

    bool add_browser(HWND owner) {
        auto b = make_shared<browser>("", "");
        auto error = make_shared<string>();
        return form_dialog(owner, "Add custom browser", 600, 420, [b, error](form& f, function<void()> rebuild) {
            f.text("Name", b->name);
            f.combo("Engine", {"Generic", "Chromium based", "Gecko (Firefox) based"},
                [b] { return static_cast<int>(b->engine); },
                [b, rebuild](int i) {
                    b->engine = static_cast<browser_engine>(i);
                    b->set_management(b->engine);
                    rebuild();
                });
            HWND exe = f.text("Executable", b->open_cmd, false, {}, 90);
            f.button("Browse...", [b, exe, rebuild] {
                string path = open_file_dialog(::GetParent(exe), L"Browser executable", {{L"Programs", L"*.exe"}});
                if(path.empty()) return;
                b->open_cmd = path;
                wstring product = win32::user::get_file_version_info_string(path, "ProductName");
                if(!product.empty() && b->name.empty()) b->name = str::to_str(product);
                rebuild();
            }, true, scale(84, f.get_dpi()));

            if(b->engine != browser_engine::generic) {
                HWND data = f.text("Data folder", b->data_path, false, {}, 90);
                f.button("Browse...", [b, data, rebuild] {
                    string path = open_folder_dialog(::GetParent(data), L"Browser data folder");
                    if(path.empty()) return;
                    b->data_path = path;
                    rebuild();
                }, true, scale(84, f.get_dpi()));
                f.label("Chromium: the \"User Data\" folder. Firefox: the folder holding profiles.ini.", true, 2);
            }
            if(!error->empty()) f.label(*error, false, 3);
        }, [b, error, owner] {
            b->set_management(b->engine);
            string& e = *error;
            e.clear();
            if(b->open_cmd.empty()) e += "Executable is required. ";
            else if(!filesystem::exists(b->open_cmd)) e += "Executable does not exist. ";
            if(b->name.empty()) e += "Name is required. ";
            const bool needs_data = b->engine != browser_engine::generic;
            if(needs_data) {
                if(b->data_path.empty()) {
                    e += "Data folder is required. ";
                } else {
                    b->profiles.clear();
                    discovery::discover_managed_profiles(*b);
                    // at least a private profile and one real profile
                    if(b->profiles.size() < 2) e += "No profiles found in the data folder. ";
                }
            }
            if(!e.empty()) {
                message_box(owner, "Add custom browser", e, MB_OK | MB_ICONWARNING);
                return false;
            }
            b->profiles.clear();
            if(b->management == management_extent::none) discovery::discover_other_profiles(*b);
            else discovery::discover_managed_profiles(*b);
            g_state.browsers.push_back(*b);
            host::state_changed();
            return true;
        });
    }

    // ---------------------------------------------------------------- process picker

    string pick_process_name(HWND owner) {
        vector<string> names;
        for(process& p : process::enumerate()) {
            if(!p.is_valid()) continue;
            string n = p.get_name();
            if(!n.empty()) names.push_back(n);
        }
        ranges::sort(names, [](const string& a, const string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
        names.erase(unique(names.begin(), names.end()), names.end());

        UINT dpi = owner ? dpi_of(owner) : ::GetDpiForSystem();
        const int w = scale(360, dpi), h = scale(460, dpi);
        POINT pos = centred(owner, w, h);
        auto chosen = make_shared<string>();
        auto filter_edit = make_shared<HWND>();
        auto list = make_shared<HWND>();

        auto refill = [names, filter_edit, list] {
            string filter = get_text(*filter_edit);
            ::SendMessageW(*list, LB_RESETCONTENT, 0, 0);
            for(auto& n : names) {
                if(filter.empty() || str::contains_ic(n, filter))
                    ::SendMessageW(*list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(n).c_str()));
            }
            ::SendMessageW(*list, LB_SETCURSEL, 0, 0);
        };
        auto take = [chosen, list](HWND hwnd) {
            int sel = static_cast<int>(::SendMessageW(*list, LB_GETCURSEL, 0, 0));
            if(sel >= 0) {
                int len = static_cast<int>(::SendMessageW(*list, LB_GETTEXTLEN, sel, 0));
                wstring t(len + 1, L'\0');
                ::SendMessageW(*list, LB_GETTEXT, sel, reinterpret_cast<LPARAM>(t.data()));
                t.resize(len);
                *chosen = narrow(t);
            }
            ::DestroyWindow(hwnd);
        };

        HWND wnd = create_window(WS_EX_DLGMODALFRAME, WS_POPUP | WS_CAPTION | WS_SYSMENU, "Running processes", owner,
            pos.x, pos.y, w, h, [refill, take, list](HWND hwnd, UINT m, WPARAM wp, LPARAM) -> optional<LRESULT> {
                if(m == WM_COMMAND) {
                    if(LOWORD(wp) == 10 && HIWORD(wp) == EN_CHANGE) {
                        refill();
                        return 0;
                    }
                    if(LOWORD(wp) == IDOK || (LOWORD(wp) == 11 && HIWORD(wp) == LBN_DBLCLK)) {
                        take(hwnd);
                        return 0;
                    }
                    if(LOWORD(wp) == IDCANCEL) {
                        ::DestroyWindow(hwnd);
                        return 0;
                    }
                }
                if(m == WM_DESTROY) unregister_dialog(hwnd);
                return nullopt;
            });
        RECT rc;
        ::GetClientRect(wnd, &rc);
        const int m = scale(12, dpi), row = scale(24, dpi), bh = scale(28, dpi), bw = scale(88, dpi);
        *filter_edit = child(wnd, L"Edit", "", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 10, dpi);
        ::MoveWindow(*filter_edit, m, m, rc.right - m * 2, row, FALSE);
        ::SendMessageW(*filter_edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Filter"));
        *list = child(wnd, L"ListBox", "", LBS_NOTIFY | WS_VSCROLL | WS_TABSTOP | LBS_NOINTEGRALHEIGHT, WS_EX_CLIENTEDGE, 11, dpi);
        ::MoveWindow(*list, m, m * 2 + row, rc.right - m * 2, rc.bottom - m * 4 - row - bh, FALSE);
        HWND ok = child(wnd, L"Button", "OK", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, IDOK, dpi);
        ::MoveWindow(ok, rc.right - m - bw, rc.bottom - m - bh, bw, bh, FALSE);
        refill();
        register_dialog(wnd);
        ::ShowWindow(wnd, SW_SHOW);
        ::SetFocus(*filter_edit);
        run_modal(wnd, owner);
        return *chosen;
    }

    // ---------------------------------------------------------------- substitutions

    void show_substitutions(HWND owner) {
        UINT dpi = owner ? dpi_of(owner) : ::GetDpiForSystem();
        const int w = scale(640, dpi), h = scale(460, dpi);
        POINT pos = centred(owner, w, h);

        struct ctx_t {
            HWND lv{}, in{}, out{};
            UINT dpi{96};
        };
        auto c = make_shared<ctx_t>();
        c->dpi = dpi;

        auto refresh = [c] {
            ::SendMessageW(c->lv, LVM_DELETEALLITEMS, 0, 0);
            int i = 0;
            for(auto& s : g_state.transforms.substitutions) lv_row(c->lv, i++, {s.kind, s.find, s.replace});
            click_payload cp;
            cp.url = get_text(c->in);
            if(!cp.url.empty()) g_pipeline.process(cp);
            set_text(c->out, cp.url);
        };
        auto edit_one = [](HWND owner_wnd, substitition_state& s) {
            auto tmp = make_shared<substitition_state>(s);
            if(tmp->kind.empty()) tmp->kind = "string";
            bool ok = form_dialog(owner_wnd, "Substitution", 500, 280, [tmp](form& f, function<void()>) {
                f.combo("Kind", {"string", "regex"},
                    [tmp] { return tmp->kind == "regex" ? 1 : 0; },
                    [tmp](int i) { tmp->kind = i == 1 ? "regex" : "string"; });
                f.text("Find", tmp->find);
                f.text("Replace with", tmp->replace);
            });
            if(ok) s = *tmp;
            return ok;
        };

        enum { ID_LV = 20, ID_ADD, ID_EDIT, ID_DEL, ID_IN, ID_HELP };
        HWND wnd = create_window(WS_EX_DLGMODALFRAME, WS_POPUP | WS_CAPTION | WS_SYSMENU, "Substitutions", owner,
            pos.x, pos.y, w, h, [c, refresh, edit_one](HWND hwnd, UINT m, WPARAM wp, LPARAM lp) -> optional<LRESULT> {
                auto& subs = g_state.transforms.substitutions;
                auto changed = [&] {
                    host::state_changed(true);
                    refresh();
                };
                if(m == WM_COMMAND) {
                    switch(LOWORD(wp)) {
                        case ID_ADD: {
                            substitition_state s{"string", "", ""};
                            if(edit_one(hwnd, s)) {
                                subs.push_back(s);
                                changed();
                            }
                            return 0;
                        }
                        case ID_EDIT: {
                            int i = lv_selected(c->lv);
                            if(i >= 0 && i < static_cast<int>(subs.size()) && edit_one(hwnd, subs[i])) changed();
                            return 0;
                        }
                        case ID_DEL: {
                            int i = lv_selected(c->lv);
                            if(i >= 0 && i < static_cast<int>(subs.size())) {
                                subs.erase(subs.begin() + i);
                                changed();
                            }
                            return 0;
                        }
                        case ID_IN:
                            if(HIWORD(wp) == EN_CHANGE) refresh();
                            return 0;
                        case ID_HELP:
                            shell_open(string{APP_HELP_BASE_URL} + "#substitutions");
                            return 0;
                        case IDCANCEL:
                        case IDOK:
                            ::DestroyWindow(hwnd);
                            return 0;
                        default:
                            break;
                    }
                }
                if(m == WM_NOTIFY) {
                    auto* hdr = reinterpret_cast<NMHDR*>(lp);
                    if(hdr->hwndFrom == c->lv && hdr->code == NM_DBLCLK) {
                        ::SendMessageW(hwnd, WM_COMMAND, ID_EDIT, 0);
                        return 0;
                    }
                }
                if(m == WM_DESTROY) unregister_dialog(hwnd);
                return nullopt;
            });

        RECT rc;
        ::GetClientRect(wnd, &rc);
        const int m = scale(12, dpi), row = scale(24, dpi), bw = scale(88, dpi), bh = scale(28, dpi), lw = scale(70, dpi);
        int y = m;
        HWND l1 = child(wnd, L"Static", "Test link", SS_CENTERIMAGE, 0, -1, dpi);
        ::MoveWindow(l1, m, y, lw, row, FALSE);
        c->in = child(wnd, L"Edit", "", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, ID_IN, dpi);
        ::MoveWindow(c->in, m + lw, y, rc.right - m * 2 - lw, row, FALSE);
        y += row + scale(6, dpi);
        HWND l2 = child(wnd, L"Static", "Result", SS_CENTERIMAGE, 0, -1, dpi);
        ::MoveWindow(l2, m, y, lw, row, FALSE);
        c->out = child(wnd, L"Edit", "", ES_AUTOHSCROLL | ES_READONLY, WS_EX_CLIENTEDGE, -1, dpi);
        ::MoveWindow(c->out, m + lw, y, rc.right - m * 2 - lw, row, FALSE);
        y += row + m;

        int bx = m;
        for(auto [id, text] : {pair{ID_ADD, "Add..."}, pair{ID_EDIT, "Edit..."}, pair{ID_DEL, "Delete"}, pair{ID_HELP, "Help"}}) {
            HWND b = child(wnd, L"Button", text, BS_PUSHBUTTON | WS_TABSTOP, 0, id, dpi);
            ::MoveWindow(b, bx, y, bw, bh, FALSE);
            bx += bw + scale(6, dpi);
        }
        y += bh + scale(6, dpi);
        c->lv = make_list_view(wnd, ID_LV, dpi);
        ::MoveWindow(c->lv, m, y, rc.right - m * 2, rc.bottom - y - m * 2 - bh, FALSE);
        lv_columns(c->lv, {{"Kind", 70}, {"Find", 240}, {"Replace with", 240}}, dpi);
        HWND close = child(wnd, L"Button", "Close", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, IDOK, dpi);
        ::MoveWindow(close, rc.right - m - bw, rc.bottom - m - bh, bw, bh, FALSE);

        refresh();
        register_dialog(wnd);
        ::ShowWindow(wnd, SW_SHOW);
        run_modal(wnd, owner);
    }

    // ---------------------------------------------------------------- script editor

    void show_script_editor() {
        static HWND open_wnd{nullptr};
        if(open_wnd && ::IsWindow(open_wnd)) {
            ::ShowWindow(open_wnd, SW_RESTORE);
            ::SetForegroundWindow(open_wnd);
            return;
        }

        enum { ID_SAVE = 30, ID_TPL_RULE, ID_TPL_PPL, ID_FN, ID_RUN, ID_HELP, ID_CODE, ID_URL, ID_TITLE, ID_PROC, ID_OUT };
        struct ctx_t {
            HWND code{}, out{}, fn{}, err{}, url{}, title{}, proc{};
            vector<HWND> bar;
            vector<HWND> labels;
            UINT dpi{96};
        };
        auto c = make_shared<ctx_t>();
        UINT dpi{96};
        RECT work = cursor_work_area(&dpi);
        c->dpi = dpi;

        auto refill_functions = [c] {
            ::SendMessageW(c->fn, CB_RESETCONTENT, 0, 0);
            for(auto& n : g_script.bt_function_names)
                ::SendMessageW(c->fn, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(n).c_str()));
            ::SendMessageW(c->fn, CB_SETCURSEL, 0, 0);
            set_text(c->err, g_script.get_error());
        };

        auto save = [c, refill_functions] {
            string code = get_text(c->code);
            str::replace_all(code, "\r\n", "\n");
            g_script.set_code(code);
            host::state_changed(true);
            refill_functions();
        };

        auto layout = [c](HWND hwnd) {
            RECT rc;
            ::GetClientRect(hwnd, &rc);
            const UINT d = c->dpi;
            const int m = scale(8, d), row = scale(26, d), lw = scale(90, d);
            int x = m;
            for(HWND b : c->bar) {
                int w = b == c->fn ? scale(220, d) : scale(110, d);
                ::MoveWindow(b, x, m, w, b == c->fn ? scale(300, d) : row, TRUE);
                x += w + scale(4, d);
            }
            int y = m + row + m;
            HWND inputs[] = {c->url, c->title, c->proc};
            for(int i = 0; i < 3; i++) {
                ::MoveWindow(c->labels[i], m, y, lw, row, TRUE);
                ::MoveWindow(inputs[i], m + lw, y, rc.right - m * 2 - lw, row, TRUE);
                y += row + scale(4, d);
            }
            ::MoveWindow(c->err, m, y, rc.right - m * 2, row, TRUE);
            y += row;
            int out_h = (rc.bottom - y) / 4;
            ::MoveWindow(c->code, m, y, rc.right - m * 2, rc.bottom - y - out_h - m * 2, TRUE);
            ::MoveWindow(c->out, m, rc.bottom - out_h - m, rc.right - m * 2, out_h, TRUE);
        };

        auto run = [c, save] {
            save();
            if(!g_script.get_error().empty()) return;
            int sel = static_cast<int>(::SendMessageW(c->fn, CB_GETCURSEL, 0, 0));
            if(sel < 0 || sel >= static_cast<int>(g_script.bt_function_names.size())) return;
            string fn = g_script.bt_function_names[sel];

            click_payload up;
            up.url = g_state.pipevis.url;
            up.window_title = g_state.pipevis.window_title;
            up.process_name = g_state.pipevis.process_name;
            string out = format("calling {} with\r\n  url: [{}]\r\n  window title: [{}]\r\n  process name: [{}]\r\n\r\n",
                fn, up.url, up.window_title, up.process_name);
            g_script.print_buffer.clear();
            if(fn.starts_with(LuaPipelinePrefix)) {
                string result = g_script.call_ppl(up, fn);
                out += g_script.print_buffer;
                out += format("\r\nURL transformed to:\r\n  [{}]\r\n", result);
            } else {
                g_pipeline.process(up);
                if(up.url != g_state.pipevis.url) out += format("pipeline changed URL to [{}]\r\n", up.url);
                g_script.print_buffer.clear();
                bool matched = g_script.call_rule(up, fn);
                out += g_script.print_buffer;
                out += format("\r\nrule matched: {}\r\n", matched);
            }
            str::replace_all(out, "\r\n", "\n");
            str::replace_all(out, "\n", "\r\n");
            set_text(c->out, out);
            set_text(c->err, g_script.get_error());
        };

        const int w = scale(900, dpi), h = scale(700, dpi);
        HWND wnd = create_window(0, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, strings::ScriptEditor + " - scripts.lua", nullptr,
            work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2, w, h,
            [c, save, run, layout](HWND hwnd, UINT m, WPARAM wp, LPARAM) -> optional<LRESULT> {
                switch(m) {
                    case WM_SIZE:
                        if(c->code) layout(hwnd);
                        return 0;
                    case WM_COMMAND:
                        switch(LOWORD(wp)) {
                            case ID_SAVE:
                                save();
                                return 0;
                            case ID_RUN:
                                run();
                                return 0;
                            case ID_TPL_RULE:
                            case ID_TPL_PPL: {
                                string code = get_text(c->code);
                                code += "\r\n\r\n";
                                code += LOWORD(wp) == ID_TPL_RULE ? strings::LuaScriptNewRule : strings::LuaScriptNewPipeline;
                                str::replace_all(code, "\r\n", "\n");
                                str::replace_all(code, "\n", "\r\n");
                                set_text(c->code, code);
                                save();
                                return 0;
                            }
                            case ID_HELP:
                                shell_open(string{APP_HELP_BASE_URL} + "#scripting");
                                return 0;
                            case ID_URL:
                            case ID_TITLE:
                            case ID_PROC:
                                if(HIWORD(wp) == EN_CHANGE) {
                                    g_state.pipevis.url = get_text(c->url);
                                    g_state.pipevis.window_title = get_text(c->title);
                                    g_state.pipevis.process_name = get_text(c->proc);
                                    host::state_changed();
                                }
                                return 0;
                            default:
                                break;
                        }
                        break;
                    case WM_DESTROY:
                        unregister_dialog(hwnd);
                        break;
                    default:
                        break;
                }
                return nullopt;
            });
        open_wnd = wnd;

        for(auto [id, text] : {pair{ID_SAVE, "Save"}, pair{ID_TPL_RULE, "+ Rule"}, pair{ID_TPL_PPL, "+ Pipeline step"}}) {
            c->bar.push_back(child(wnd, L"Button", text, BS_PUSHBUTTON | WS_TABSTOP, 0, id, dpi));
        }
        c->fn = child(wnd, L"ComboBox", "", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 0, ID_FN, dpi);
        c->bar.push_back(c->fn);
        c->bar.push_back(child(wnd, L"Button", "Run", BS_PUSHBUTTON | WS_TABSTOP, 0, ID_RUN, dpi));
        c->bar.push_back(child(wnd, L"Button", "Help", BS_PUSHBUTTON | WS_TABSTOP, 0, ID_HELP, dpi));
        for(const char* l : {"Test URL", "Window title", "Process"})
            c->labels.push_back(child(wnd, L"Static", l, SS_CENTERIMAGE, 0, -1, dpi));
        c->url = child(wnd, L"Edit", g_state.pipevis.url, ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, ID_URL, dpi);
        c->title = child(wnd, L"Edit", g_state.pipevis.window_title, ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, ID_TITLE, dpi);
        c->proc = child(wnd, L"Edit", g_state.pipevis.process_name, ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, ID_PROC, dpi);
        c->err = child(wnd, L"Static", "", SS_CENTERIMAGE | SS_NOPREFIX, 0, -1, dpi);

        string code = g_script.get_code();
        str::replace_all(code, "\r\n", "\n");
        str::replace_all(code, "\n", "\r\n");
        c->code = child(wnd, L"Edit", code, ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP,
            WS_EX_CLIENTEDGE, ID_CODE, dpi);
        ::SendMessageW(c->code, WM_SETFONT, reinterpret_cast<WPARAM>(mono_font(dpi)), FALSE);
        ::SendMessageW(c->code, EM_SETLIMITTEXT, 0, 0);
        c->out = child(wnd, L"Edit", "", ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL, WS_EX_CLIENTEDGE, ID_OUT, dpi);
        ::SendMessageW(c->out, WM_SETFONT, reinterpret_cast<WPARAM>(mono_font(dpi)), FALSE);

        refill_functions();
        layout(wnd);
        register_dialog(wnd);
        ::ShowWindow(wnd, SW_SHOW);
        ::SetForegroundWindow(wnd);
    }

    // ---------------------------------------------------------------- pipeline debugger

    void show_pipeline_debugger() {
        static HWND open_wnd{nullptr};
        if(open_wnd && ::IsWindow(open_wnd)) {
            ::ShowWindow(open_wnd, SW_RESTORE);
            ::SetForegroundWindow(open_wnd);
            return;
        }

        enum { ID_URL = 40, ID_TITLE, ID_PROC, ID_ONLY, ID_LV };
        struct ctx_t {
            HWND url{}, title{}, proc{}, only{}, lv{};
            vector<HWND> labels;
            UINT dpi{96};
        };
        auto c = make_shared<ctx_t>();
        UINT dpi{96};
        RECT work = cursor_work_area(&dpi);
        c->dpi = dpi;

        auto refresh = [c] {
            click_payload cp;
            cp.url = g_state.pipevis.url;
            cp.window_title = g_state.pipevis.window_title;
            cp.process_name = g_state.pipevis.process_name;
            auto steps = g_pipeline.process_debug(cp);
            const bool only = ::SendMessageW(c->only, BM_GETCHECK, 0, 0) == BST_CHECKED;

            ::SendMessageW(c->lv, WM_SETREDRAW, FALSE, 0);
            ::SendMessageW(c->lv, LVM_DELETEALLITEMS, 0, 0);
            int row = 0;
            lv_row(c->lv, row++, {"URL", g_state.pipevis.url});
            if(!steps.empty()) {
                lv_row(c->lv, row++, {"Pipeline", ""});
                for(auto& s : steps) {
                    bool modified = s.before.url != s.after.url;
                    lv_row(c->lv, row++, {"    " + url_pipeline_step::to_string(s.step->type), (modified ? "changed: " : "") + s.after.url});
                }
            }
            lv_row(c->lv, row++, {"Result", cp.url});
            lv_row(c->lv, row++, {"Browsers", ""});
            for(auto& b : g_state.browsers) {
                vector<pair<string, string>> lines;
                bool b_match{false};
                for(auto& p : b.profiles) {
                    vector<pair<string, string>> rule_lines;
                    bool p_match{false};
                    for(auto& r : p.rules) {
                        bool m = r.is_match(cp, g_script);
                        p_match |= m;
                        if(!only || m) rule_lines.emplace_back(format("            {}{}", m ? "✓ " : "", r.get_type_string()), r.to_string(false));
                    }
                    b_match |= p_match;
                    if(only && !p_match) continue;
                    lines.emplace_back(format("        {}{}", p_match ? "✓ " : "", p.name),
                        p.rules.empty() ? "no rules" : format("{} rule(s)", p.rules.size()));
                    lines.insert(lines.end(), rule_lines.begin(), rule_lines.end());
                }
                if(only && !b_match) continue;
                lv_row(c->lv, row++, {format("    {}{}", b_match ? "✓ " : "", b.name), ""});
                for(auto& [k, v] : lines) lv_row(c->lv, row++, {k, v});
            }
            ::SendMessageW(c->lv, WM_SETREDRAW, TRUE, 0);
        };

        auto layout = [c](HWND hwnd) {
            RECT rc;
            ::GetClientRect(hwnd, &rc);
            const UINT d = c->dpi;
            const int m = scale(8, d), row = scale(24, d), lw = scale(90, d);
            int y = m;
            HWND inputs[] = {c->url, c->title, c->proc};
            for(int i = 0; i < 3; i++) {
                ::MoveWindow(c->labels[i], m, y, lw, row, TRUE);
                ::MoveWindow(inputs[i], m + lw, y, rc.right - m * 2 - lw, row, TRUE);
                y += row + scale(4, d);
            }
            ::MoveWindow(c->only, m + lw, y, scale(300, d), row, TRUE);
            y += row + scale(4, d);
            ::MoveWindow(c->lv, m, y, rc.right - m * 2, rc.bottom - y - m, TRUE);
        };

        const int w = scale(860, dpi), h = scale(600, dpi);
        HWND wnd = create_window(0, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, strings::PipelineDebugger, nullptr,
            work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2, w, h,
            [c, refresh, layout](HWND hwnd, UINT m, WPARAM wp, LPARAM) -> optional<LRESULT> {
                switch(m) {
                    case WM_SIZE:
                        if(c->lv) layout(hwnd);
                        return 0;
                    case WM_COMMAND:
                        if((LOWORD(wp) == ID_URL || LOWORD(wp) == ID_TITLE || LOWORD(wp) == ID_PROC) && HIWORD(wp) == EN_CHANGE) {
                            g_state.pipevis.url = get_text(c->url);
                            g_state.pipevis.window_title = get_text(c->title);
                            g_state.pipevis.process_name = get_text(c->proc);
                            host::state_changed();
                            refresh();
                            return 0;
                        }
                        if(LOWORD(wp) == ID_ONLY) {
                            refresh();
                            return 0;
                        }
                        break;
                    case WM_DESTROY:
                        unregister_dialog(hwnd);
                        break;
                    default:
                        break;
                }
                return nullopt;
            });
        open_wnd = wnd;

        for(const char* l : {"URL", "Window title", "Process"})
            c->labels.push_back(child(wnd, L"Static", l, SS_CENTERIMAGE, 0, -1, dpi));
        c->url = child(wnd, L"Edit", g_state.pipevis.url, ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, ID_URL, dpi);
        c->title = child(wnd, L"Edit", g_state.pipevis.window_title, ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, ID_TITLE, dpi);
        c->proc = child(wnd, L"Edit", g_state.pipevis.process_name, ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, ID_PROC, dpi);
        c->only = child(wnd, L"Button", "Show matching browsers only", BS_AUTOCHECKBOX | WS_TABSTOP, 0, ID_ONLY, dpi);
        c->lv = make_list_view(wnd, ID_LV, dpi);
        lv_columns(c->lv, {{"Step", 320}, {"Value", 480}}, dpi);

        layout(wnd);
        refresh();
        register_dialog(wnd);
        ::ShowWindow(wnd, SW_SHOW);
        ::SetForegroundWindow(wnd);
    }
}
