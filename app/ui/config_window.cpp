#include "config_window.h"
#include <map>
#include <memory>
#include <format>
#include <functional>
#include <filesystem>
#include <commctrl.h>
#include <commdlg.h>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#include "form.h"
#include "win32ui.h"
#include "icons.h"
#include "tools.h"
#include "globals.h"
#include "../host.h"
#include "../setup.h"
#include "../strings.h"
#include "../url_opener.h"
#include "common/str.h"
#include "common/fss.h"

using namespace std;
using namespace grey::common;

namespace bt::ui::config_window {

    namespace {
        // ---- look

        constexpr COLORREF C_BG = RGB(243, 243, 243);
        constexpr COLORREF C_CARD = RGB(255, 255, 255);
        constexpr COLORREF C_CARD_BORDER = RGB(229, 229, 229);
        constexpr COLORREF C_LINE = RGB(237, 237, 237);
        constexpr COLORREF C_TEXT = RGB(26, 26, 26);
        constexpr COLORREF C_DIM = RGB(96, 96, 96);
        constexpr COLORREF C_ACCENT = RGB(0, 95, 184);
        constexpr COLORREF C_FIELD = RGB(255, 255, 255);
        constexpr COLORREF C_FIELD_RO = RGB(249, 249, 249);
        constexpr COLORREF C_FIELD_BORDER = RGB(211, 211, 211);
        constexpr COLORREF C_NAV_SEL = RGB(234, 234, 234);
        constexpr COLORREF C_WARN = RGB(196, 43, 28);
        constexpr COLORREF C_OK = RGB(15, 123, 15);

        // Segoe Fluent Icons / MDL2
        constexpr wchar_t G_GLOBE[] = L"";
        constexpr wchar_t G_WINDOW[] = L"";
        constexpr wchar_t G_CHIP[] = L"";
        constexpr wchar_t G_CODE[] = L"";
        constexpr wchar_t G_HOME[] = L"";
        constexpr wchar_t G_FOLDER[] = L"";
        constexpr wchar_t G_APP[] = L"";
        constexpr wchar_t G_EDIT[] = L"";
        constexpr wchar_t G_DELETE[] = L"";
        constexpr wchar_t G_ADD[] = L"";
        constexpr wchar_t G_UP[] = L"";
        constexpr wchar_t G_DOWN[] = L"";
        constexpr wchar_t G_LEFT[] = L"";
        constexpr wchar_t G_RIGHT[] = L"";
        constexpr wchar_t G_STAR[] = L"";
        constexpr wchar_t G_STAR_FILLED[] = L"";
        constexpr wchar_t G_WARN[] = L"";
        constexpr wchar_t G_CHECK[] = L"";

        constexpr UINT WM_APP_REBUILD = WM_APP + 20;

        HBRUSH brush(COLORREF c) {
            static map<COLORREF, HBRUSH> cache;
            auto it = cache.find(c);
            if(it == cache.end()) it = cache.emplace(c, ::CreateSolidBrush(c)).first;
            return it->second;
        }

        Gdiplus::Color gp(COLORREF c) {
            return {255, GetRValue(c), GetGValue(c), GetBValue(c)};
        }

        void rounded(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r) {
            float d = r * 2;
            path.AddArc(x, y, d, d, 180, 90);
            path.AddArc(x + w - d, y, d, d, 270, 90);
            path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
            path.AddArc(x, y + h - d, d, d, 90, 90);
            path.CloseFigure();
        }

        // ---- painted page content

        struct text_item {
            RECT r;
            wstring s;
            HFONT font;
            COLORREF colour;
            UINT flags;
        };

        struct glyph_item {
            RECT r;
            wstring g;
            COLORREF colour;
            int size_pt;
        };

        struct bitmap_item {
            int x, y;
            HBITMAP bmp;
        };

        struct section_t {
            string id;
            string title;
            int indent;
            int y;
            int browser{-1};    // index into g_state.browsers for browser and profile anchors
            int profile{-1};    // profile anchors are listed in the navigation only under the current browser
        };

        enum class ctl_kind { toggle, glyph_button, other };

        struct control_info {
            ctl_kind kind{ctl_kind::other};
            function<bool()> get;           // toggles
            wstring glyph;                  // glyph buttons
            function<void(int code)> on;    // WM_COMMAND notification code
        };

        struct window_t {
            HWND wnd{}, view{}, page{}, tooltip{};
            UINT dpi{96};
            int scroll{0};
            int page_h{0};
            int built_width{0};
            bool sizing{false};     // inside a resize drag
            vector<RECT> cards, lines, fields, ro_fields;
            vector<text_item> texts;
            vector<glyph_item> glyphs;
            vector<bitmap_item> bitmaps;
            vector<section_t> sections;
            vector<HWND> controls;
            map<int, control_info> infos;
            int next_id{1000};
            vector<system_check> health;
        } w;

        int S(int v) { return scale(v, w.dpi); }

        void rebuild();
        void request_rebuild() { ::PostMessageW(w.wnd, WM_APP_REBUILD, 0, 0); }

        void changed() { host::state_changed(); }

        // ---- page builder

        /**
         * @brief Places rows and cards top to bottom. Text is recorded for painting; controls are created
         * on the page. Every row has a title and an optional description on the left and its control on the right.
         */
        class builder {
        public:
            builder(int width) : width{width} {
                margin = S(28);
                pad = S(18);
                y = S(20);
                control_w = min(S(320), (width - margin * 2) * 45 / 100);
            }

            int y;

            void section(const string& id, const string& title, int indent = 0) {
                if(!w.sections.empty() || y > S(20)) y += S(18);
                w.sections.push_back({id, title, indent, y});
                text(margin, y, width - margin * 2, S(34), title, ui_font(w.dpi, 6, true), C_TEXT);
                y += S(42);
            }

            void subheading(const string& title) {
                text(margin + S(4), y, width - margin * 2, S(24), title, ui_font(w.dpi, 1, true), C_TEXT);
                y += S(28);
            }

            void begin_card() {
                card_top = y;
                first_row = true;
            }

            void end_card() {
                w.cards.push_back({margin, card_top, width - margin, y});
                y += S(10);
            }

            /**
             * @brief A row; returns the rectangle reserved for the control at the right.
             */
            RECT row(const string& title, const string& desc = "", int lead_px = 0, HBITMAP lead = nullptr,
                const wchar_t* lead_glyph = nullptr, bool bold = false, int extra_h = 0) {
                const int h = (desc.empty() ? S(50) : S(64)) + extra_h;
                if(!first_row) w.lines.push_back({margin + pad, y, width - margin - pad, y + 1});
                first_row = false;

                int x = margin + pad + indent;
                if(lead) {
                    w.bitmaps.push_back({x, y + (h - lead_px) / 2, lead});
                    x += lead_px + S(12);
                } else if(lead_glyph) {
                    w.glyphs.push_back({{x, y, x + S(20), y + h}, lead_glyph, C_TEXT, 12});
                    x += S(32);
                }
                const int text_right = width - margin - pad - control_w - S(12);
                HFONT tf = ui_font(w.dpi, bold ? 1 : 0, bold);
                if(desc.empty()) {
                    text(x, y, text_right - x, h, title, tf, C_TEXT, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                } else {
                    const int th = S(20), dh = S(18);
                    const int top = y + (h - th - dh) / 2;
                    text(x, top, text_right - x, th, title, tf, C_TEXT, DT_SINGLELINE | DT_END_ELLIPSIS | DT_BOTTOM);
                    text(x, top + th, text_right - x, dh, desc, ui_font(w.dpi, -1), C_DIM, DT_SINGLELINE | DT_END_ELLIPSIS | DT_TOP);
                }
                RECT c{width - margin - pad - control_w, y, width - margin - pad, y + h};
                y += h;
                return c;
            }

            /**
             * @brief A row whose control takes most of the width (paths, arguments).
             */
            RECT wide_row(const string& title, int buttons_w = 0) {
                const int h = S(50);
                if(!first_row) w.lines.push_back({margin + pad, y, width - margin - pad, y + 1});
                first_row = false;
                const int x = margin + pad + indent;
                const int label_w = S(150);
                text(x, y, label_w, h, title, ui_font(w.dpi), C_TEXT, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                RECT c{x + label_w, y, width - margin - pad - buttons_w, y + h};
                y += h;
                return c;
            }

            int indent{0};

        private:
            int width, margin, pad, control_w;
            int card_top{0};
            bool first_row{true};

            static void text(int x, int y, int cw, int ch, const string& s, HFONT f, COLORREF c,
                UINT flags = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS) {
                w.texts.push_back({{x, y, x + cw, y + ch}, wide(s), f, c, flags | DT_NOPREFIX});
            }
        };

        // ---- controls on the page

        HWND add_control(const wchar_t* cls, const string& text, DWORD style, RECT r, control_info info, DWORD ex = 0) {
            int id = w.next_id++;
            HWND h = child(w.page, cls, text, style, ex, id, w.dpi);
            ::MoveWindow(h, r.left, r.top, r.right - r.left, r.bottom - r.top, FALSE);
            w.controls.push_back(h);
            w.infos[id] = std::move(info);
            return h;
        }

        void tip(HWND control, const string& text) {
            if(!text.empty()) add_tooltip(w.tooltip, w.page, control, text);
        }

        /**
         * @brief A Windows 11 style on/off switch, right-aligned in the control area.
         */
        HWND toggle(RECT area, function<bool()> get, function<void(bool)> set) {
            const int tw = S(96), th = S(28);
            RECT r{area.right - tw, area.top + (area.bottom - area.top - th) / 2, area.right, 0};
            r.bottom = r.top + th;
            control_info info;
            info.kind = ctl_kind::toggle;
            info.get = get;
            info.on = [get, set](int code) {
                // owner-drawn buttons report a fast second click as BN_DOUBLECLICKED
                if(code == BN_CLICKED || code == BN_DOUBLECLICKED) set(!get());
            };
            HWND h = add_control(L"Button", "", BS_OWNERDRAW | WS_TABSTOP, r, info);
            return h;
        }

        HWND toggle(RECT area, bool& value, function<void()> after = changed) {
            bool* v = &value;
            return toggle(area, [v] { return *v; }, [v, after](bool b) {
                *v = b;
                if(after) after();
            });
        }

        /**
         * @brief A small square button showing a glyph. Buttons are laid out right to left.
         */
        HWND glyph_button(int& right, int top, int bottom, const wchar_t* glyph, const string& tooltip,
            function<void()> click, bool enabled = true) {
            const int bw = S(32);
            RECT r{right - bw, top + (bottom - top - bw) / 2, right, 0};
            r.bottom = r.top + bw;
            right -= bw + S(4);
            control_info info;
            info.kind = ctl_kind::glyph_button;
            info.glyph = glyph;
            info.on = [click](int code) {
                if((code == BN_CLICKED || code == BN_DOUBLECLICKED) && click) click();
            };
            HWND h = add_control(L"Button", "", BS_OWNERDRAW | WS_TABSTOP, r, info);
            ::EnableWindow(h, enabled);
            tip(h, tooltip);
            return h;
        }

        int text_px(const string& s) {
            HDC hdc = ::GetDC(nullptr);
            HGDIOBJ old = ::SelectObject(hdc, ui_font(w.dpi));
            SIZE sz{};
            wstring ws = wide(s);
            ::GetTextExtentPoint32W(hdc, ws.c_str(), static_cast<int>(ws.size()), &sz);
            ::SelectObject(hdc, old);
            ::ReleaseDC(nullptr, hdc);
            return sz.cx;
        }

        /**
         * @brief A push button; laid out right to left like glyph buttons.
         */
        HWND push_button(int& right, int top, int bottom, const string& text, function<void()> click, bool enabled = true) {
            const int bw = max(S(88), text_px(text) + S(28)), bh = S(32);
            RECT r{right - bw, top + (bottom - top - bh) / 2, right, 0};
            r.bottom = r.top + bh;
            right -= bw + S(6);
            control_info info;
            info.on = [click](int code) {
                if(code == BN_CLICKED && click) click();
            };
            HWND h = add_control(L"Button", text, BS_PUSHBUTTON | WS_TABSTOP, r, info);
            ::EnableWindow(h, enabled);
            return h;
        }

        /**
         * @brief A text field: a painted rounded box with a borderless one-line edit centred inside.
         */
        HWND field(RECT area, string& value, bool read_only = false, function<void()> after = changed) {
            const int bh = S(32);
            RECT box{area.left, area.top + (area.bottom - area.top - bh) / 2, area.right, 0};
            box.bottom = box.top + bh;
            (read_only ? w.ro_fields : w.fields).push_back(box);
            const int text_h = [] {
                HDC hdc = ::GetDC(nullptr);
                HGDIOBJ old = ::SelectObject(hdc, ui_font(w.dpi));
                TEXTMETRICW tm{};
                ::GetTextMetricsW(hdc, &tm);
                ::SelectObject(hdc, old);
                ::ReleaseDC(nullptr, hdc);
                return static_cast<int>(tm.tmHeight);
            }();
            RECT r{box.left + S(10), box.top + (bh - text_h) / 2, box.right - S(10), 0};
            r.bottom = r.top + text_h;
            string* v = &value;
            auto handle = make_shared<HWND>();
            control_info info;
            info.on = [v, handle, after](int code) {
                if(code != EN_CHANGE) return;
                *v = get_text(*handle);
                if(after) after();
            };
            *handle = add_control(L"Edit", value, ES_AUTOHSCROLL | WS_TABSTOP | (read_only ? ES_READONLY : 0), r, info);
            return *handle;
        }

        template<typename T>
        HWND number(RECT area, T& value, T lo, T hi) {
            const int fw = S(110);
            RECT r{area.right - fw, area.top, area.right, area.bottom};
            string text = is_same_v<T, float> ? format("{:g}", static_cast<double>(value)) : to_string(value);
            const int bh = S(32);
            RECT box{r.left, r.top + (r.bottom - r.top - bh) / 2, r.right, 0};
            box.bottom = box.top + bh;
            w.fields.push_back(box);
            HDC hdc = ::GetDC(nullptr);
            HGDIOBJ old = ::SelectObject(hdc, ui_font(w.dpi));
            TEXTMETRICW tm{};
            ::GetTextMetricsW(hdc, &tm);
            ::SelectObject(hdc, old);
            ::ReleaseDC(nullptr, hdc);
            RECT er{box.left + S(10), box.top + (bh - tm.tmHeight) / 2, box.right - S(10), 0};
            er.bottom = er.top + tm.tmHeight;
            T* v = &value;
            auto handle = make_shared<HWND>();
            control_info info;
            info.on = [v, lo, hi, handle](int code) {
                if(code != EN_CHANGE) return;
                try {
                    double d = stod(get_text(*handle));
                    if(d < lo || d > hi) return;
                    *v = static_cast<T>(d);
                    changed();
                } catch(...) {
                }
            };
            *handle = add_control(L"Edit", text, ES_AUTOHSCROLL | ES_RIGHT | WS_TABSTOP, er, info);
            return *handle;
        }

        LRESULT CALLBACK combo_wheel(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
            // a closed combo box would change its value on the mouse wheel; scroll the page instead
            if(m == WM_MOUSEWHEEL && !::SendMessageW(h, CB_GETDROPPEDSTATE, 0, 0)) {
                return ::SendMessageW(w.view, m, wp, lp);
            }
            return ::DefSubclassProc(h, m, wp, lp);
        }

        HWND combo(RECT area, const vector<string>& items, function<int()> get, function<void(int)> set) {
            const int ch = S(32);
            RECT r{area.left, area.top + (area.bottom - area.top - ch) / 2, area.right, 0};
            r.bottom = r.top + S(300);
            auto handle = make_shared<HWND>();
            control_info info;
            info.on = [handle, set](int code) {
                if(code == CBN_SELCHANGE) set(static_cast<int>(::SendMessageW(*handle, CB_GETCURSEL, 0, 0)));
            };
            HWND h = add_control(L"ComboBox", "", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, r, info);
            *handle = h;
            for(auto& s : items) ::SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(s).c_str()));
            ::SendMessageW(h, CB_SETCURSEL, get(), 0);
            // keep the combo's own height, which centres its text, and centre it in the row
            RECT wr{};
            ::GetWindowRect(h, &wr);
            const int closed_h = wr.bottom - wr.top;
            ::MoveWindow(h, r.left, area.top + (area.bottom - area.top - closed_h) / 2, r.right - r.left, S(300), FALSE);
            ::SetWindowSubclass(h, combo_wheel, 1, 0);
            return h;
        }

        // ---- sections

        void build_status(builder& b) {
            b.section("status", "Status");
            b.begin_card();
            w.health = setup::get_checks();
            bool ok = true;
            for(auto& c : w.health) {
                c.recheck();
                ok &= c.is_ok;
            }
            RECT r = b.row("Default browser",
                ok ? "Windows sends links to " APP_LONG_NAME "." : "Windows does not send links to " APP_LONG_NAME " yet.",
                0, nullptr, ok ? G_CHECK : G_WARN);
            w.glyphs.back().colour = ok ? C_OK : C_WARN;
            if(!ok) {
                int right = r.right;
                push_button(right, r.top, r.bottom, "Fix...", [] {
                    for(auto& c : w.health) if(!c.is_ok) c.fix();
                });
            }
            size_t profiles{0}, rules{0};
            for(auto& br : g_state.browsers) {
                profiles += br.profiles.size();
                for(auto& p : br.profiles) rules += p.rules.size();
            }
            string def;
            if(auto sel = browser::get_default(g_state.browsers)) {
                def = sel->b().name;
                if(sel->b().engine != browser_engine::generic) def += " / " + sel->p().name;
            }
            b.row(format("{} browsers, {} profiles, {} rules", g_state.browsers.size(), profiles, rules),
                def.empty() ? "" : "Default profile: " + def);
            b.end_card();
        }

        const wchar_t* location_glyph(match_location loc) {
            switch(loc) {
                case match_location::url: return G_GLOBE;
                case match_location::window_title: return G_WINDOW;
                case match_location::process_name: return G_CHIP;
                case match_location::lua_script: return G_CODE;
            }
            return G_GLOBE;
        }

        string rule_description(const browser& br, const match_rule& r) {
            string d;
            switch(r.loc) {
                case match_location::url:
                    d = r.scope == match_scope::domain ? "Host name contains" : r.scope == match_scope::path ? "Path contains" : "URL contains";
                    break;
                case match_location::window_title: d = "Window title contains"; break;
                case match_location::process_name: d = "Process name contains"; break;
                case match_location::lua_script: d = "Lua function returns true"; break;
            }
            if(r.is_regex) d = d.starts_with("Lua") ? d : d.substr(0, d.find(" contains")) + " matches regex";
            if(r.app_mode && br.supports_frameless_windows()) d += " · opens as app window";
            return d;
        }

        void build_rules(builder& b, browser& br, browser_profile& p) {
            for(size_t i = 0; i < p.rules.size(); i++) {
                match_rule& r = p.rules[i];
                RECT c = b.row(r.value, rule_description(br, r), 0, nullptr, location_glyph(r.loc));
                // small badges after the value: scope and regex
                const wchar_t* scope = r.loc == match_location::url && r.scope == match_scope::domain ? G_HOME
                                     : r.loc == match_location::url && r.scope == match_scope::path ? G_FOLDER : nullptr;
                int right = c.right;
                const int n = static_cast<int>(p.rules.size());
                glyph_button(right, c.top, c.bottom, G_DELETE, "Delete rule", [&p, i] {
                    p.rules.erase(p.rules.begin() + static_cast<long>(i));
                    changed();
                    request_rebuild();
                });
                glyph_button(right, c.top, c.bottom, G_DOWN, "Move down", [&p, i] {
                    swap(p.rules[i], p.rules[i + 1]);
                    changed();
                    request_rebuild();
                }, static_cast<int>(i) + 1 < n);
                glyph_button(right, c.top, c.bottom, G_UP, "Move up", [&p, i] {
                    swap(p.rules[i], p.rules[i - 1]);
                    changed();
                    request_rebuild();
                }, i > 0);
                glyph_button(right, c.top, c.bottom, G_EDIT, "Edit rule", [&br, &p, i] {
                    if(edit_rule(w.wnd, br, p.rules[i])) {
                        changed();
                        request_rebuild();
                    }
                });
                // badges sit just left of the buttons
                int bx = right - S(4);
                auto badge = [&](const wchar_t* g, COLORREF col) {
                    w.glyphs.push_back({{bx - S(22), c.top, bx, c.bottom}, g, col, 10});
                    bx -= S(26);
                };
                if(r.app_mode && br.supports_frameless_windows()) badge(G_APP, C_DIM);
                if(scope) badge(scope, C_DIM);
                if(r.is_regex) {
                    w.texts.push_back({{bx - S(22), c.top, bx, c.bottom}, L".*", ui_font(w.dpi, 1, true), C_DIM,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE});
                }
            }
            RECT c = b.row(p.rules.empty() ? "No rules yet" : "", p.rules.empty() ? "Links that match a rule open here without the picker." : "");
            int right = c.right;
            push_button(right, c.top, c.bottom, "Add rule...", [&br, &p] {
                match_rule r;
                if(edit_rule(w.wnd, br, r)) {
                    p.rules.push_back(r);
                    changed();
                    request_rebuild();
                }
            });
        }

        void pick_colour(browser_profile& p) {
            static COLORREF custom[16]{};
            CHOOSECOLORW cc{sizeof(cc)};
            cc.hwndOwner = w.wnd;
            cc.rgbResult = RGB(p.user_color.r8(), p.user_color.g8(), p.user_color.b8());
            cc.lpCustColors = custom;
            cc.Flags = CC_RGBINIT | CC_FULLOPEN;
            bool chosen{false};
            {
                modal_scope scope;
                chosen = ::ChooseColorW(&cc) != FALSE;
            }
            if(chosen) {
                p.user_color = grey::rgb_colour::from_bytes(GetRValue(cc.rgbResult), GetGValue(cc.rgbResult), GetBValue(cc.rgbResult));
                p.use_user_color = true;
                icons::clear();
                changed();
                request_rebuild();
            }
        }

        void build_profile(builder& b, browser& br, int pi) {
            browser_profile& p = br.profiles[pi];
            const bool managed = br.management != management_extent::none;
            const int n = static_cast<int>(br.profiles.size());

            string desc;
            if(p.is_default) desc = "Default";
            if(p.is_incognito) desc += string{desc.empty() ? "" : " · "} + "Private";
            desc += string{desc.empty() ? "" : " · "} + format("{} {}", p.rules.size(), p.rules.size() == 1 ? "rule" : "rules");
            if(p.is_hidden) desc += " · hidden in picker";

            const int icon_px = S(32);
            if(managed) w.sections.push_back({format("profile:{}:{}", &br - g_state.browsers.data(), pi), p.name, 1, b.y,
                static_cast<int>(&br - g_state.browsers.data()), pi});
            RECT c = b.row(managed ? p.name : "Launch", desc, icon_px,
                icons::profile_bitmap(br, p, icon_px, icon_overlay_mode::profile_only), nullptr, true);
            int right = c.right;
            if(managed) {
                toggle({right - S(96), c.top, right, c.bottom}, [&p] { return !p.is_hidden; }, [&p](bool v) {
                    p.is_hidden = !v;
                    changed();
                    if(!g_state.show_hidden_browsers) request_rebuild();
                    else ::InvalidateRect(w.wnd, nullptr, FALSE);   // navigation icons
                });
                right -= S(104);
                glyph_button(right, c.top, c.bottom, G_RIGHT, "Move right in the picker", [&br, pi] {
                    swap(br.profiles[pi], br.profiles[pi + 1]);
                    changed();
                    request_rebuild();
                }, pi + 1 < n);
                glyph_button(right, c.top, c.bottom, G_LEFT, "Move left in the picker", [&br, pi] {
                    swap(br.profiles[pi], br.profiles[pi - 1]);
                    changed();
                    request_rebuild();
                }, pi > 0);
            }
            glyph_button(right, c.top, c.bottom, p.is_default ? G_STAR_FILLED : G_STAR,
                p.is_default ? "Default profile" : "Make default", [&p] {
                    browser::set_default(g_state.browsers, p);
                    changed();
                    request_rebuild();
                }, !p.is_default);
            glyph_button(right, c.top, c.bottom, G_APP, "Open a test link", [&br, pi] {
                url_opener::open(profile_selection(br, pi), APP_TEST_URL);
            });

            b.indent = S(44);
            if(!br.is_internal()) {
                if(managed) {
                    field(b.wide_row("Arguments"), p.launch_arg, true);
                    field(b.wide_row("Extra arguments"), p.user_arg);
                } else {
                    field(b.wide_row("Arguments"), p.launch_arg);
                    toggle(b.row("Hide the program's window"), p.no_window);
                }
                RECT ic = b.row("Icon", p.user_icon_path.empty() ? "Built-in" : p.user_icon_path);
                int ir = ic.right;
                push_button(ir, ic.top, ic.bottom, "Reset", [&p] {
                    p.user_icon_path.clear();
                    icons::clear();
                    changed();
                    request_rebuild();
                }, !p.user_icon_path.empty());
                push_button(ir, ic.top, ic.bottom, "Change...", [&p] {
                    string path = open_file_dialog(w.wnd, L"Profile icon", {{L"Images", L"*.png;*.ico;*.jpg;*.bmp;*.exe"}});
                    if(path.empty()) return;
                    p.user_icon_path = path;
                    icons::clear();
                    changed();
                    request_rebuild();
                });
                RECT cc = b.row("Highlight colour", "A coloured ring around the icon in the picker");
                int cr = cc.right;
                if(p.use_user_color) {
                    push_button(cr, cc.top, cc.bottom, "Pick...", [&p] { pick_colour(p); });
                }
                toggle({cr - S(96), cc.top, cr, cc.bottom}, [&p] { return p.use_user_color; }, [&p](bool v) {
                    p.use_user_color = v;
                    icons::clear();
                    changed();
                    request_rebuild();
                });
            }
            build_rules(b, br, p);
            b.indent = 0;
        }

        void build_browsers(builder& b) {
            b.section("browsers", "Browsers");
            b.begin_card();
            RECT c = b.row("Show hidden browsers and profiles", "Hidden ones never appear in the picker");
            int right = c.right;
            toggle({right - S(96), c.top, right, c.bottom}, g_state.show_hidden_browsers, [] {
                changed();
                request_rebuild();
            });
            right -= S(104);
            push_button(right, c.top, c.bottom, "Rediscover", [] { host::rediscover(); });
            push_button(right, c.top, c.bottom, "Add...", [] {
                if(add_browser(w.wnd)) request_rebuild();
            });
            b.end_card();

            const int nb = static_cast<int>(g_state.browsers.size());
            for(int bi = 0; bi < nb; bi++) {
                browser& br = g_state.browsers[bi];
                if(br.is_hidden && !g_state.show_hidden_browsers) continue;
                const bool full = br.management == management_extent::full;

                w.sections.push_back({format("browser:{}", bi), br.name, 0, b.y, bi, -1});
                b.begin_card();
                string engine = br.engine == browser_engine::chromium ? "Chromium based"
                              : br.engine == browser_engine::gecko ? "Gecko based" : "Generic";
                string managed = full ? "found by discovery"
                               : br.management == management_extent::profiles ? "added by hand, profiles discovered" : "added by hand";
                const int icon_px = S(36);
                RECT hc = b.row(br.name, format("{} · {} · {} {}", engine, managed, br.profiles.size(),
                    br.profiles.size() == 1 ? "profile" : "profiles"), icon_px,
                    icons::path_bitmap(br.get_best_icon_path(), icon_px, false), nullptr, true, S(8));
                int hr = hc.right;
                toggle({hr - S(96), hc.top, hr, hc.bottom}, [&br] { return !br.is_hidden; }, [&br](bool v) {
                    br.is_hidden = !v;
                    changed();
                    if(!g_state.show_hidden_browsers) request_rebuild();
                });
                hr -= S(104);
                glyph_button(hr, hc.top, hc.bottom, G_DELETE, "Delete browser", [bi] {
                    {
                        modal_scope scope;
                        if(::MessageBoxW(w.wnd, L"Delete this browser and all its rules?", L"Delete", MB_YESNO | MB_ICONWARNING) != IDYES) return;
                    }
                    g_state.browsers.erase(g_state.browsers.begin() + bi);
                    changed();
                    request_rebuild();
                });
                glyph_button(hr, hc.top, hc.bottom, G_DOWN, "Move down in the picker", [bi] {
                    swap(g_state.browsers[bi], g_state.browsers[bi + 1]);
                    changed();
                    request_rebuild();
                }, bi + 1 < nb);
                glyph_button(hr, hc.top, hc.bottom, G_UP, "Move up in the picker", [bi] {
                    swap(g_state.browsers[bi], g_state.browsers[bi - 1]);
                    changed();
                    request_rebuild();
                }, bi > 0);

                if(!full) field(b.wide_row("Name"), br.name);
                if(!br.is_internal()) {
                    const int bw = S(96);
                    RECT fc = b.wide_row("Command", bw);
                    field(fc, br.open_cmd, full);
                    int fr = fc.right + bw;
                    push_button(fr, fc.top, fc.bottom, "Folder", [&br] {
                        shell_open(filesystem::path{br.open_cmd}.parent_path().string());
                    });
                    if(!br.data_path.empty()) {
                        RECT dc = b.wide_row("Data folder", bw);
                        field(dc, br.data_path, full);
                        int dr = dc.right + bw;
                        push_button(dr, dc.top, dc.bottom, "Open", [&br] { shell_open(br.data_path); });
                    }
                }
                if(br.engine == browser_engine::gecko) {
                    toggle(b.row("Discover legacy profiles"), br.discover_classic_gecko_profiles);
                    toggle(b.row("Discover containers"), br.discover_containers);
                    RECT pc = b.row("Firefox profiles", "Manage profiles in Firefox itself");
                    int pr = pc.right;
                    push_button(pr, pc.top, pc.bottom, "about:profiles", [&br] { shell_open(br.open_cmd, "about:profiles"); });
                    push_button(pr, pc.top, pc.bottom, "Profile manager", [&br] { shell_open(br.open_cmd, "-P"); });
                }

                for(int pi = 0; pi < static_cast<int>(br.profiles.size()); pi++) {
                    if(br.profiles[pi].is_hidden && !g_state.show_hidden_browsers && br.management != management_extent::none) continue;
                    build_profile(b, br, pi);
                    if(br.management == management_extent::none) break;   // one profile, edited as the browser
                }
                b.end_card();
            }
        }

        void build_picker(builder& b) {
            auto& p = g_state.picker;
            b.section("picker", "Picker");
            b.subheading("Behaviour");
            b.begin_card();
            toggle(b.row("Close when it loses focus"), p.close_on_focus_loss);
            toggle(b.row("Always on top"), p.always_on_top);
            toggle(b.row("Show when no rule matches", "Otherwise the default profile opens the link"), p.invoke.on_no_rule);
            toggle(b.row("Show key hints", "Numbers 1-0 on the first ten profiles"), p.show_key_hints);
            toggle(b.row("Window frame", "A normal title bar and border"), p.show_native_chrome);
            b.end_card();

            b.subheading("Open the picker when these keys are held on click");
            b.begin_card();
            toggle(b.row("Ctrl + Shift"), p.invoke.on_key_control_shift);
            toggle(b.row("Ctrl + Alt"), p.invoke.on_key_control_alt);
            toggle(b.row("Alt + Shift"), p.invoke.on_key_alt_shift);
            toggle(b.row("Caps Lock on"), p.invoke.on_key_caps_locks);
            b.end_card();

            b.subheading("Look");
            b.begin_card();
            combo(b.row("Labels"), {"None", "Browser and profile", "Browser", "Profile"},
                [&p] { return static_cast<int>(p.label_display); },
                [&p](int i) { p.label_display = static_cast<label_display_mode>(i); changed(); });
            combo(b.row("Icons", "How browser and profile icons combine"),
                {"Profile on top of browser", "Browser on top of profile", "Browser only", "Profile only"},
                [] { return static_cast<int>(g_state.icon_overlay); },
                [](int i) { g_state.icon_overlay = static_cast<icon_overlay_mode>(i); changed(); });
            number(b.row("Item size", "16 to 256"), p.box_size, 16.0f, 256.0f);
            number(b.row("Item padding", "0 to 100"), p.item_padding, 0.0f, 100.0f);
            number(b.row("Item rounding", "0 to 128"), p.item_rounding, 0.0f, 128.0f);
            number(b.row("Label size", "-15 to 15, relative to the normal text"), p.label_size, -15.0f, 15.0f);
            number(b.row("Minimum width", "Percent of the screen"), p.min_width_perc, 10, 90);
            number(b.row("Maximum width", "Percent of the screen"), p.max_width_perc, 20, 100);
            number(b.row("Border width"), p.border_width, 0, 10);
            number(b.row("Opacity", "0.1 to 1"), p.opacity, 0.1f, 1.0f);
            number(b.row("Colour ring width"), g_state.highlight_width, 0, 20);
            RECT c = b.row("Reset", "Back to the default look and behaviour");
            int right = c.right;
            push_button(right, c.top, c.bottom, "Reset picker", [] {
                g_state.picker = picker_state{};
                changed();
                request_rebuild();
            });
            b.end_card();
        }

        void build_toast(builder& b) {
            auto& t = g_state.toast;
            b.section("toast", "Toast");
            b.begin_card();
            toggle(b.row("Show a toast when a rule opens a link", "Says where the click came from and where it went"), t.enabled);
            number(b.row("Visible for", "Seconds"), t.visible_seconds, 0, 60);
            number(b.row("Fade", "Seconds"), t.anim_duration, 0.0f, 5.0f);
            number(b.row("Border width"), t.border_width, 0, 6);
            number(b.row("Opacity", "0.2 to 1"), t.opacity, 0.2f, 1.0f);
            b.end_card();
        }

        void build_cleanup(builder& b) {
            auto& t = g_state.transforms;
            auto reload = [] { host::state_changed(true); };
            b.section("cleanup", "Link cleanup");
            b.begin_card();
            toggle(b.row("Unwrap Office 365 safe links"), t.unwrap_o365, reload);
            toggle(b.row("Unshorten short links", "Follows t.co, bit.ly and similar"), t.unshorten, reload);
            toggle(b.row("Remove trackers", "ClearURLs rules"), t.clearurls, reload);
            RECT c = b.row("Substitute text in links", "Find and replace, plain or regex");
            int right = c.right;
            toggle({right - S(96), c.top, right, c.bottom}, t.substitute, reload);
            right -= S(104);
            push_button(right, c.top, c.bottom, "Edit...", [] { show_substitutions(w.wnd); });
            c = b.row("Lua pipeline functions", "ppl_* functions in scripts.lua");
            right = c.right;
            toggle({right - S(96), c.top, right, c.bottom}, t.scripting, reload);
            right -= S(104);
            push_button(right, c.top, c.bottom, "Script editor", [] { show_script_editor(); });
            b.end_card();
        }

        void build_general(builder& b) {
            b.section("general", "General");
            b.begin_card();
            toggle(b.row("Run at Windows sign-in", "Keeps the first click fast"), g_state.run_at_startup);
            toggle(b.row("Tray icon"), g_state.show_tray_icon);
            toggle(b.row("Rediscover browsers when settings open"), g_state.rediscover_on_config_startup);
            toggle(b.row("Discover the Black Hole browser", "A browser that opens nothing"), g_state.discover_black_hole_browser, [] {
                changed();
                host::rediscover();
            });
            toggle(b.row("Log clicks", "clicks_<month>.csv in the configuration folder"), g_state.log_rule_hits);
            toggle(b.row("Log picker timing", "timing.log in the configuration folder"), g_state.log_timing);
            b.end_card();
        }

        void build_tools(builder& b) {
            b.section("tools", "Tools");
            b.begin_card();
            auto tool = [&](const string& title, const string& desc, const string& button, function<void()> fn) {
                RECT c = b.row(title, desc);
                int right = c.right;
                push_button(right, c.top, c.bottom, button, fn);
            };
            tool("Windows default apps", "Make " APP_LONG_NAME " the default browser", "Open", [] { shell_open("ms-settings:defaultapps"); });
            tool(strings::PipelineDebugger, "See how a link is cleaned and which rules match", "Open", [] { show_pipeline_debugger(); });
            tool(strings::ScriptEditor, "Edit scripts.lua", "Open", [] { show_script_editor(); });
            tool("Configuration folder", fss::get_config_dir(CONFIG_NAME), "Open", [] { shell_open(fss::get_config_dir(CONFIG_NAME)); });
            tool("Exit " APP_LONG_NAME, "Stops the background instance until the next click", "Exit", [] {
                ::DestroyWindow(w.wnd);
                host::quit();
            });
            b.end_card();
        }

        void build_about(builder& b) {
            b.section("about", "About");
            b.begin_card();
            RECT c = b.row(format("{} {}", APP_LONG_NAME, APP_VERSION), "Native Win32, resident in the background, fast", S(32),
                icons::path_bitmap("logo", S(32), false));
            int right = c.right;
            push_button(right, c.top, c.bottom, "Releases", [] { shell_open(APP_GITHUB_RELEASES_URL); });
            RECT a = b.row("Based on Browser Tamer by Ivan G (aloneguid)",
                APP_LONG_NAME " is a fork of Browser Tamer under the Apache 2.0 licence. Thank you, Ivan.");
            int ar = a.right;
            push_button(ar, a.top, a.bottom, "Donate", [] { shell_open(APP_DONATE_URL); });
            push_button(ar, a.top, a.bottom, "Browser Tamer", [] { shell_open(APP_UPSTREAM_URL); });
            RECT d = b.row("Documentation", "The Browser Tamer manual covers rules, scripting and the command line");
            int dr = d.right;
            push_button(dr, d.top, d.bottom, "Open", [] { shell_open(APP_DOCS_URL); });
            string timings = host::timing_summary();
            int lines = static_cast<int>(ranges::count(timings, '\n')) + 1;
            RECT t = b.row("Recent clicks", "Time from click to picker or browser start, newest first", 0, nullptr, nullptr, false,
                max(0, S(18) * min(lines, 8) - S(10)));
            (void)t;
            w.texts.push_back({{t.left, t.top + S(12), t.right, t.bottom - S(8)}, wide(timings), mono_font(w.dpi), C_DIM, DT_LEFT | DT_NOPREFIX});
            b.end_card();
        }

        // ---- page painting

        void paint_page(HDC target, const RECT& clip) {
            const int cw = clip.right - clip.left, ch = clip.bottom - clip.top;
            if(cw <= 0 || ch <= 0) return;
            HDC hdc = ::CreateCompatibleDC(target);
            HBITMAP bmp = ::CreateCompatibleBitmap(target, cw, ch);
            HGDIOBJ old = ::SelectObject(hdc, bmp);
            ::SetViewportOrgEx(hdc, -clip.left, -clip.top, nullptr);

            ::FillRect(hdc, &clip, brush(C_BG));
            {
                Gdiplus::Graphics g{hdc};
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                // the DC's viewport origin already maps page coordinates into the buffer
                Gdiplus::SolidBrush card{gp(C_CARD)};
                Gdiplus::Pen border{gp(C_CARD_BORDER), 1.0f};
                const float r = static_cast<float>(S(8));
                for(const RECT& c : w.cards) {
                    if(c.bottom < clip.top || c.top > clip.bottom) continue;
                    Gdiplus::GraphicsPath path;
                    rounded(path, c.left + 0.5f, c.top + 0.5f, static_cast<float>(c.right - c.left - 1), static_cast<float>(c.bottom - c.top - 1), r);
                    g.FillPath(&card, &path);
                    g.DrawPath(&border, &path);
                }
                Gdiplus::Pen field_border{gp(C_FIELD_BORDER), 1.0f};
                Gdiplus::SolidBrush field{gp(C_FIELD)}, field_ro{gp(C_FIELD_RO)};
                const float fr = static_cast<float>(S(5));
                for(auto* list : {&w.fields, &w.ro_fields}) {
                    for(const RECT& f : *list) {
                        if(f.bottom < clip.top || f.top > clip.bottom) continue;
                        Gdiplus::GraphicsPath path;
                        rounded(path, f.left + 0.5f, f.top + 0.5f, static_cast<float>(f.right - f.left - 1), static_cast<float>(f.bottom - f.top - 1), fr);
                        g.FillPath(list == &w.fields ? &field : &field_ro, &path);
                        g.DrawPath(&field_border, &path);
                    }
                }
            }
            for(const RECT& l : w.lines) {
                if(l.top >= clip.top && l.top <= clip.bottom) ::FillRect(hdc, &l, brush(C_LINE));
            }
            ::SetBkMode(hdc, TRANSPARENT);
            for(auto& t : w.texts) {
                if(t.r.bottom < clip.top || t.r.top > clip.bottom) continue;
                HGDIOBJ of = ::SelectObject(hdc, t.font);
                ::SetTextColor(hdc, t.colour);
                RECT r = t.r;
                ::DrawTextW(hdc, t.s.c_str(), -1, &r, t.flags);
                ::SelectObject(hdc, of);
            }
            for(auto& gl : w.glyphs) {
                if(gl.r.bottom < clip.top || gl.r.top > clip.bottom) continue;
                HGDIOBJ of = ::SelectObject(hdc, icon_font(w.dpi, gl.size_pt));
                ::SetTextColor(hdc, gl.colour);
                RECT r = gl.r;
                ::DrawTextW(hdc, gl.g.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                ::SelectObject(hdc, of);
            }
            for(auto& bm : w.bitmaps) {
                if(bm.y > clip.bottom || bm.y + S(64) < clip.top) continue;
                icons::draw(hdc, bm.bmp, bm.x, bm.y);
            }

            ::SetViewportOrgEx(hdc, 0, 0, nullptr);
            ::BitBlt(target, clip.left, clip.top, cw, ch, hdc, 0, 0, SRCCOPY);
            ::SelectObject(hdc, old);
            ::DeleteObject(bmp);
            ::DeleteDC(hdc);
        }

        void draw_control(const DRAWITEMSTRUCT& di) {
            auto it = w.infos.find(static_cast<int>(di.CtlID));
            if(it == w.infos.end()) return;
            const control_info& info = it->second;
            // draw into a buffer and copy once, so the control never shows a half-drawn state
            const RECT target = di.rcItem;
            const int bw = target.right - target.left, bh = target.bottom - target.top;
            HDC hdc = ::CreateCompatibleDC(di.hDC);
            HBITMAP buf = ::CreateCompatibleBitmap(di.hDC, bw, bh);
            HGDIOBJ old_buf = ::SelectObject(hdc, buf);
            RECT rc{0, 0, bw, bh};
            ::FillRect(hdc, &rc, brush(C_CARD));
            const bool disabled = di.itemState & ODS_DISABLED;
            const bool pressed = di.itemState & ODS_SELECTED;
            const bool focus = di.itemState & ODS_FOCUS;

            if(info.kind == ctl_kind::toggle) {
                const bool on = info.get && info.get();
                const int sw = S(40), sh = S(20);
                const int sx = rc.right - sw - S(2), sy = rc.top + (rc.bottom - rc.top - sh) / 2;
                {
                    Gdiplus::Graphics g{hdc};
                    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                    Gdiplus::GraphicsPath track;
                    rounded(track, sx + 0.5f, sy + 0.5f, static_cast<float>(sw - 1), static_cast<float>(sh - 1), (sh - 1) / 2.0f);
                    if(on) {
                        Gdiplus::SolidBrush fill{gp(C_ACCENT)};
                        g.FillPath(&fill, &track);
                    } else {
                        Gdiplus::SolidBrush fill{gp(C_CARD)};
                        Gdiplus::Pen pen{gp(RGB(134, 134, 134)), 1.0f};
                        g.FillPath(&fill, &track);
                        g.DrawPath(&pen, &track);
                    }
                    const float knob = sh * (pressed ? 0.6f : 0.5f);
                    const float kx = on ? sx + sw - sh / 2.0f - knob / 2 : sx + sh / 2.0f - knob / 2;
                    const float ky = sy + sh / 2.0f - knob / 2;
                    Gdiplus::SolidBrush kb{on ? Gdiplus::Color{255, 255, 255, 255} : gp(RGB(95, 95, 95))};
                    g.FillEllipse(&kb, kx, ky, knob, knob);
                }
                RECT tr{rc.left, rc.top, sx - S(10), rc.bottom};
                ::SetBkMode(hdc, TRANSPARENT);
                ::SetTextColor(hdc, C_TEXT);
                HGDIOBJ of = ::SelectObject(hdc, ui_font(w.dpi));
                ::DrawTextW(hdc, on ? L"On" : L"Off", -1, &tr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                ::SelectObject(hdc, of);
            } else if(info.kind == ctl_kind::glyph_button) {
                if(pressed) {
                    Gdiplus::Graphics g{hdc};
                    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                    Gdiplus::GraphicsPath path;
                    rounded(path, rc.left + 0.5f, rc.top + 0.5f, static_cast<float>(rc.right - rc.left - 1), static_cast<float>(rc.bottom - rc.top - 1), static_cast<float>(S(4)));
                    Gdiplus::SolidBrush fill{gp(C_NAV_SEL)};
                    g.FillPath(&fill, &path);
                }
                ::SetBkMode(hdc, TRANSPARENT);
                ::SetTextColor(hdc, disabled ? RGB(190, 190, 190) : C_TEXT);
                HGDIOBJ of = ::SelectObject(hdc, icon_font(w.dpi, 11));
                ::DrawTextW(hdc, info.glyph.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                ::SelectObject(hdc, of);
            }
            if(focus && !(di.itemState & ODS_NOFOCUSRECT)) {
                RECT fr = rc;
                ::InflateRect(&fr, -1, -1);
                ::DrawFocusRect(hdc, &fr);
            }
            ::BitBlt(di.hDC, target.left, target.top, bw, bh, hdc, 0, 0, SRCCOPY);
            ::SelectObject(hdc, old_buf);
            ::DeleteObject(buf);
            ::DeleteDC(hdc);
        }

        // ---- caption, scrolling and navigation

        // The window has no title bar. The top strip is drawn by us: dragging it moves the window and
        // the three caption buttons sit at its right end.
        int caption_h() { return S(40); }
        int nav_width() { return S(272); }
        int nav_top() { return S(84); }

        enum caption_button { cap_none = -1, cap_min = 0, cap_max, cap_close };
        int cap_hover{cap_none};

        RECT caption_button_rect(int i) {
            RECT rc;
            ::GetClientRect(w.wnd, &rc);
            const int bw = S(46);
            const int left = rc.right - bw * (3 - i);
            return {left, 0, left + bw, S(32)};
        }

        int caption_hit(POINT pt) {
            for(int i = cap_min; i <= cap_close; i++) {
                RECT r = caption_button_rect(i);
                if(::PtInRect(&r, pt)) return i;
            }
            return cap_none;
        }

        RECT view_rect() {
            RECT rc;
            ::GetClientRect(w.wnd, &rc);
            return {nav_width(), caption_h(), rc.right, rc.bottom};
        }

        int view_height() {
            RECT v = view_rect();
            return v.bottom - v.top;
        }

        int current_section() {
            int idx = 0;
            for(int i = 0; i < static_cast<int>(w.sections.size()); i++) {
                if(w.sections[i].y <= w.scroll + S(40)) idx = i;
            }
            return idx;
        }

        struct nav_item {
            int section;
            RECT r;
        };

        /**
         * @brief Navigation entries: every section and browser, plus the profiles of the browser in view.
         */
        vector<nav_item> nav_items() {
            vector<nav_item> items;
            const int cur = current_section();
            const int cur_browser = cur < static_cast<int>(w.sections.size()) ? w.sections[cur].browser : -1;
            int y = nav_top();
            for(int i = 0; i < static_cast<int>(w.sections.size()); i++) {
                const auto& sec = w.sections[i];
                if(sec.profile >= 0 && sec.browser != cur_browser) continue;
                const int h = sec.profile >= 0 ? S(30) : S(36);
                items.push_back({i, {S(10), y, nav_width() - S(10), y + h}});
                y += h;
            }
            return items;
        }

        void set_scroll(int y) {
            const int max_scroll = max(0, w.page_h - view_height());
            w.scroll = clamp(y, 0, max_scroll);
            ::SetWindowPos(w.page, nullptr, 0, -w.scroll, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            SCROLLINFO si{sizeof(si), SIF_ALL};
            si.nMin = 0;
            si.nMax = max(0, w.page_h - 1);
            si.nPage = static_cast<UINT>(view_height());
            si.nPos = w.scroll;
            ::SetScrollInfo(w.view, SB_VERT, &si, TRUE);
            RECT nav{0, 0, nav_width(), 30000};
            ::InvalidateRect(w.wnd, &nav, FALSE);
        }

        void scroll_to(const string& id) {
            for(auto& sec : w.sections) {
                if(sec.id == id) {
                    set_scroll(sec.y - S(12));
                    return;
                }
            }
        }

        void fill_rounded(HDC hdc, RECT r, float radius, COLORREF c) {
            Gdiplus::Graphics g{hdc};
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::GraphicsPath path;
            rounded(path, static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right - r.left),
                static_cast<float>(r.bottom - r.top), radius);
            Gdiplus::SolidBrush b{gp(c)};
            g.FillPath(&b, &path);
        }

        void paint_window(HDC target) {
            RECT rc;
            ::GetClientRect(w.wnd, &rc);
            HDC hdc = ::CreateCompatibleDC(target);
            HBITMAP bmp = ::CreateCompatibleBitmap(target, rc.right, max(1L, rc.bottom));
            HGDIOBJ old = ::SelectObject(hdc, bmp);
            ::FillRect(hdc, &rc, brush(C_BG));
            ::SetBkMode(hdc, TRANSPARENT);

            // app title, in the draggable strip
            icons::draw(hdc, icons::path_bitmap("logo", S(28), false), S(22), S(26));
            HGDIOBJ of = ::SelectObject(hdc, ui_font(w.dpi, 3, true));
            ::SetTextColor(hdc, C_TEXT);
            RECT tr{S(60), S(22), nav_width() - S(8), S(58)};
            ::DrawTextW(hdc, wide(APP_LONG_NAME).c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            // navigation
            const int cur = current_section();
            for(const nav_item& it : nav_items()) {
                const auto& sec = w.sections[it.section];
                RECT item = it.r;
                const bool profile = sec.profile >= 0;
                const bool active = it.section == cur ||
                    (!profile && sec.browser >= 0 && cur < static_cast<int>(w.sections.size()) && w.sections[cur].browser == sec.browser);
                if(active) {
                    RECT hr = item;
                    hr.top += S(2);
                    hr.bottom -= S(2);
                    fill_rounded(hdc, hr, static_cast<float>(S(5)), it.section == cur ? C_NAV_SEL : RGB(238, 238, 238));
                    if(it.section == cur) {
                        RECT bar{item.left, item.top + (item.bottom - item.top) * 3 / 10, item.left + S(3), item.bottom - (item.bottom - item.top) * 3 / 10};
                        fill_rounded(hdc, bar, static_cast<float>(S(1)), C_ACCENT);
                    }
                }
                int x = item.left + S(14) + (profile ? S(22) : 0);
                const int mid = (item.top + item.bottom) / 2;
                const browser* br = sec.browser >= 0 && sec.browser < static_cast<int>(g_state.browsers.size())
                    ? &g_state.browsers[sec.browser] : nullptr;
                if(br && !profile) {
                    const int ip = S(18);
                    icons::draw(hdc, icons::path_bitmap(br->get_best_icon_path(), ip, false), x, mid - ip / 2);
                    x += ip + S(10);
                } else if(br && profile && sec.profile < static_cast<int>(br->profiles.size())) {
                    const int ip = S(18);
                    icons::draw(hdc, icons::profile_bitmap(*br, br->profiles[sec.profile], ip, icon_overlay_mode::profile_only), x, mid - ip / 2);
                    x += ip + S(8);
                }

                // after a browser's name: its profiles' icons, small, as many as fit
                int text_right = item.right - S(6);
                if(br && !profile && br->management != management_extent::none) {
                    const int sp = S(14), gap = S(2);
                    int count = 0;
                    for(auto& p : br->profiles) if(!p.is_hidden || g_state.show_hidden_browsers) count++;
                    const int max_icons = 5;
                    const int shown = min(count, max_icons);
                    int ix = item.right - S(8) - shown * (sp + gap);
                    text_right = ix - S(4);
                    int n = 0;
                    for(auto& p : br->profiles) {
                        if(p.is_hidden && !g_state.show_hidden_browsers) continue;
                        if(n++ >= max_icons) break;
                        icons::draw(hdc, icons::profile_bitmap(*br, p, sp, icon_overlay_mode::profile_only), ix, mid - sp / 2);
                        ix += sp + gap;
                    }
                }

                RECT t{x, item.top, text_right, item.bottom};
                ::SelectObject(hdc, profile ? ui_font(w.dpi, -1) : ui_font(w.dpi, 1));
                ::SetTextColor(hdc, profile ? C_DIM : C_TEXT);
                wstring title = wide(sec.title);
                ::DrawTextW(hdc, title.c_str(), -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }

            // caption buttons
            const bool zoomed = ::IsZoomed(w.wnd);
            const wchar_t* glyphs[] = {L"\uE921", zoomed ? L"\uE923" : L"\uE922", L"\uE8BB"};
            for(int i = cap_min; i <= cap_close; i++) {
                RECT r = caption_button_rect(i);
                if(i == cap_hover) ::FillRect(hdc, &r, brush(i == cap_close ? RGB(196, 43, 28) : RGB(229, 229, 229)));
                ::SelectObject(hdc, icon_font(w.dpi, 7));
                ::SetTextColor(hdc, i == cap_hover && i == cap_close ? RGB(255, 255, 255) : C_TEXT);
                ::DrawTextW(hdc, glyphs[i], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }

            ::SelectObject(hdc, of);
            ::BitBlt(target, 0, 0, rc.right, rc.bottom, hdc, 0, 0, SRCCOPY);
            ::SelectObject(hdc, old);
            ::DeleteObject(bmp);
            ::DeleteDC(hdc);
        }

        // ---- build

        void rebuild() {
            if(!w.page) return;
            ::SendMessageW(w.page, WM_SETREDRAW, FALSE, 0);
            for(HWND h : w.controls) ::DestroyWindow(h);
            w.controls.clear();
            w.infos.clear();
            w.cards.clear();
            w.lines.clear();
            w.fields.clear();
            w.ro_fields.clear();
            w.texts.clear();
            w.glyphs.clear();
            w.bitmaps.clear();
            w.sections.clear();
            w.next_id = 1000;
            if(w.tooltip) ::DestroyWindow(w.tooltip);
            w.tooltip = ::CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                0, 0, 0, 0, w.page, nullptr, ::GetModuleHandleW(nullptr), nullptr);

            RECT v = view_rect();
            const int width = v.right - v.left - ::GetSystemMetricsForDpi(SM_CXVSCROLL, w.dpi);
            w.built_width = v.right - v.left;
            builder b{width};
            build_status(b);
            build_browsers(b);
            build_picker(b);
            build_toast(b);
            build_cleanup(b);
            build_general(b);
            build_tools(b);
            build_about(b);
            w.page_h = b.y + S(40);

            ::SetWindowPos(w.page, nullptr, 0, -w.scroll, width, w.page_h, SWP_NOZORDER | SWP_NOACTIVATE);
            ::SendMessageW(w.page, WM_SETREDRAW, TRUE, 0);
            set_scroll(w.scroll);
            ::RedrawWindow(w.wnd, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN);
        }

        void layout() {
            RECT v = view_rect();
            ::MoveWindow(w.view, v.left, v.top, v.right - v.left, v.bottom - v.top, TRUE);
            if(v.right - v.left != w.built_width && !w.sizing) rebuild();
            else set_scroll(w.scroll);
        }

        // ---- window procedures

        optional<LRESULT> page_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
            switch(m) {
                case WM_ERASEBKGND:
                    return 1;
                case WM_PAINT: {
                    PAINTSTRUCT ps;
                    HDC hdc = ::BeginPaint(h, &ps);
                    paint_page(hdc, ps.rcPaint);
                    ::EndPaint(h, &ps);
                    return 0;
                }
                case WM_COMMAND: {
                    auto it = w.infos.find(LOWORD(wp));
                    if(it != w.infos.end() && it->second.on) {
                        auto fn = it->second.on;   // the handler may rebuild the page
                        fn(HIWORD(wp));
                        if(it = w.infos.find(LOWORD(wp)); it != w.infos.end() && it->second.kind == ctl_kind::toggle)
                            ::InvalidateRect(reinterpret_cast<HWND>(lp), nullptr, FALSE);
                    }
                    return 0;
                }
                case WM_DRAWITEM:
                    draw_control(*reinterpret_cast<DRAWITEMSTRUCT*>(lp));
                    return TRUE;
                case WM_CTLCOLOREDIT: {
                    HDC hdc = reinterpret_cast<HDC>(wp);
                    ::SetTextColor(hdc, C_TEXT);
                    ::SetBkColor(hdc, C_FIELD);
                    return reinterpret_cast<LRESULT>(brush(C_FIELD));
                }
                case WM_CTLCOLORSTATIC: {
                    // read-only edits
                    HDC hdc = reinterpret_cast<HDC>(wp);
                    ::SetTextColor(hdc, C_DIM);
                    ::SetBkColor(hdc, C_FIELD_RO);
                    return reinterpret_cast<LRESULT>(brush(C_FIELD_RO));
                }
                case WM_CTLCOLORBTN:
                    return reinterpret_cast<LRESULT>(brush(C_CARD));
                case WM_MOUSEWHEEL:
                    return ::SendMessageW(w.view, m, wp, lp);
                default:
                    break;
            }
            return nullopt;
        }

        optional<LRESULT> view_proc(HWND, UINT m, WPARAM wp, LPARAM) {
            switch(m) {
                case WM_ERASEBKGND:
                    return 1;
                case WM_MOUSEWHEEL: {
                    const int delta = GET_WHEEL_DELTA_WPARAM(wp);
                    set_scroll(w.scroll - delta * S(120) / WHEEL_DELTA);
                    return 0;
                }
                case WM_VSCROLL: {
                    SCROLLINFO si{sizeof(si), SIF_ALL};
                    ::GetScrollInfo(w.view, SB_VERT, &si);
                    int y = w.scroll;
                    switch(LOWORD(wp)) {
                        case SB_LINEUP: y -= S(40); break;
                        case SB_LINEDOWN: y += S(40); break;
                        case SB_PAGEUP: y -= view_height(); break;
                        case SB_PAGEDOWN: y += view_height(); break;
                        case SB_THUMBTRACK:
                        case SB_THUMBPOSITION: y = si.nTrackPos; break;
                        case SB_TOP: y = 0; break;
                        case SB_BOTTOM: y = w.page_h; break;
                        default: break;
                    }
                    set_scroll(y);
                    return 0;
                }
                default:
                    break;
            }
            return nullopt;
        }

        optional<LRESULT> main_proc(HWND hwnd, UINT m, WPARAM wp, LPARAM lp) {
            switch(m) {
                case WM_ERASEBKGND:
                    return 1;
                case WM_PAINT: {
                    PAINTSTRUCT ps;
                    HDC hdc = ::BeginPaint(hwnd, &ps);
                    paint_window(hdc);
                    ::EndPaint(hwnd, &ps);
                    return 0;
                }
                case WM_SIZE:
                    // a minimised window has no client area; rebuilding for it loses the focus twice
                    if(wp == SIZE_MINIMIZED) return 0;
                    if(w.view) layout();
                    ::InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                case WM_ENTERSIZEMOVE:
                    w.sizing = true;
                    return 0;
                case WM_EXITSIZEMOVE:
                    // rebuild once when the user lets go, not for every pixel of a drag
                    w.sizing = false;
                    if(w.view) layout();
                    return 0;
                case WM_GETMINMAXINFO: {
                    auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
                    mmi->ptMinTrackSize = {S(860), S(560)};
                    return 0;
                }
                case WM_DPICHANGED: {
                    w.dpi = HIWORD(wp);
                    auto* r = reinterpret_cast<RECT*>(lp);
                    w.built_width = 0;
                    ::SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
                    return 0;
                }
                case WM_NCCALCSIZE:
                    if(wp) {
                        // keep the side and bottom frame for resizing, drop the title bar
                        auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                        const int top = p->rgrc[0].top;
                        ::DefWindowProcW(hwnd, m, wp, lp);
                        p->rgrc[0].top = top;
                        if(::IsZoomed(hwnd)) {
                            p->rgrc[0].top += ::GetSystemMetricsForDpi(SM_CYFRAME, w.dpi) +
                                              ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, w.dpi);
                        }
                        return 0;
                    }
                    break;
                case WM_NCHITTEST: {
                    LRESULT r = ::DefWindowProcW(hwnd, m, wp, lp);
                    if(r != HTCLIENT) return r;
                    POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
                    ::ScreenToClient(hwnd, &pt);
                    if(!::IsZoomed(hwnd) && pt.y < ::GetSystemMetricsForDpi(SM_CYFRAME, w.dpi)) return HTTOP;
                    if(caption_hit(pt) != cap_none) return HTCLIENT;
                    if(pt.y < caption_h() || (pt.x < nav_width() && pt.y < nav_top() - S(8))) return HTCAPTION;
                    return HTCLIENT;
                }
                case WM_MOUSEMOVE: {
                    POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
                    int hit = caption_hit(pt);
                    if(hit != cap_hover) {
                        cap_hover = hit;
                        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                        ::TrackMouseEvent(&tme);
                        RECT strip{0, 0, 30000, caption_h()};
                        ::InvalidateRect(hwnd, &strip, FALSE);
                    }
                    return 0;
                }
                case WM_MOUSELEAVE:
                    if(cap_hover != cap_none) {
                        cap_hover = cap_none;
                        RECT strip{0, 0, 30000, caption_h()};
                        ::InvalidateRect(hwnd, &strip, FALSE);
                    }
                    return 0;
                case WM_LBUTTONUP: {
                    POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
                    switch(caption_hit(pt)) {
                        case cap_min: ::ShowWindow(hwnd, SW_MINIMIZE); return 0;
                        case cap_max: ::ShowWindow(hwnd, ::IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE); return 0;
                        case cap_close: ::PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0;
                        default: break;
                    }
                    for(const nav_item& it : nav_items()) {
                        if(::PtInRect(&it.r, pt)) {
                            set_scroll(w.sections[it.section].y - S(12));
                            break;
                        }
                    }
                    return 0;
                }
                case WM_SETCURSOR: {
                    POINT pt;
                    ::GetCursorPos(&pt);
                    ::ScreenToClient(hwnd, &pt);
                    if(LOWORD(lp) == HTCLIENT) {
                        for(const nav_item& it : nav_items()) {
                            if(::PtInRect(&it.r, pt)) {
                                ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
                                return TRUE;
                            }
                        }
                    }
                    break;
                }
                case WM_MOUSEWHEEL:
                    return ::SendMessageW(w.view, m, wp, lp);
                case WM_APP_REBUILD:
                    rebuild();
                    return 0;
                case WM_ACTIVATE:
                    return nullopt;
                case WM_DESTROY:
                    unregister_dialog(hwnd);
                    w = window_t{};
                    return 0;
                default:
                    break;
            }
            return nullopt;
        }
    }

    HWND hwnd() {
        return w.wnd;
    }

    void state_reloaded() {
        if(w.wnd) rebuild();
    }

    void show(const string& section) {
        if(w.wnd) {
            if(::IsIconic(w.wnd)) ::ShowWindow(w.wnd, SW_RESTORE);
            ::SetForegroundWindow(w.wnd);
            if(!section.empty()) scroll_to(section);
            return;
        }

        if(g_state.rediscover_on_config_startup) host::rediscover();

        UINT dpi{96};
        RECT work = cursor_work_area(&dpi);
        w.dpi = dpi;
        const int ww = S(1100), wh = min(S(860), static_cast<int>(work.bottom - work.top) - S(40));
        w.wnd = create_window(0, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, format("{} settings", APP_LONG_NAME), nullptr,
            work.left + (work.right - work.left - ww) / 2, work.top + (work.bottom - work.top - wh) / 2, ww, wh, main_proc);
        // apply the frameless client area computed in WM_NCCALCSIZE
        ::SetWindowPos(w.wnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        RECT v = view_rect();
        w.view = create_window(0, WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN, "", w.wnd,
            v.left, v.top, v.right - v.left, v.bottom - v.top, view_proc);
        w.page = create_window(WS_EX_CONTROLPARENT, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, "", w.view, 0, 0, 10, 10, page_proc);
        ::SetWindowLongPtrW(w.view, GWL_EXSTYLE, ::GetWindowLongPtrW(w.view, GWL_EXSTYLE) | WS_EX_CONTROLPARENT);

        rebuild();
        if(!section.empty()) scroll_to(section);
        register_dialog(w.wnd);
        ::ShowWindow(w.wnd, SW_SHOW);
        ::SetForegroundWindow(w.wnd);
    }
}
