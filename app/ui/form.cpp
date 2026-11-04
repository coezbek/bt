#include "form.h"
#include <format>
#include <algorithm>
#include <commctrl.h>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#include "win32ui.h"

using namespace std;

namespace bt::ui {

    namespace {
        constexpr wchar_t CLASS_NAME[] = L"BrowserTamer.Window";
        constexpr wchar_t DIM_PROP[] = L"bt.dim";

        LRESULT CALLBACK window_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
            if(m == WM_NCCREATE) {
                auto* handler = static_cast<msg_handler*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
                ::SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(handler));
            }
            auto* handler = reinterpret_cast<msg_handler*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
            if(handler && *handler) {
                if(auto r = (*handler)(h, m, w, l)) return *r;
            }
            switch(m) {
                case WM_CTLCOLORSTATIC: {
                    HDC hdc = reinterpret_cast<HDC>(w);
                    HWND ctl = reinterpret_cast<HWND>(l);
                    wchar_t cls[16]{};
                    ::GetClassNameW(ctl, cls, 16);
                    if(_wcsicmp(cls, L"Edit") == 0) {
                        // read-only edit, inside a light grey field box
                        static HBRUSH ro = ::CreateSolidBrush(RGB(249, 249, 249));
                        ::SetTextColor(hdc, RGB(96, 96, 96));
                        ::SetBkColor(hdc, RGB(249, 249, 249));
                        return reinterpret_cast<LRESULT>(ro);
                    }
                    ::SetTextColor(hdc, ::GetPropW(ctl, DIM_PROP) ? ::GetSysColor(COLOR_GRAYTEXT) : ::GetSysColor(COLOR_WINDOWTEXT));
                    ::SetBkColor(hdc, ::GetSysColor(COLOR_WINDOW));
                    return reinterpret_cast<LRESULT>(::GetSysColorBrush(COLOR_WINDOW));
                }
                case WM_CLOSE:
                    ::DestroyWindow(h);
                    return 0;
                case WM_NCDESTROY:
                    ::SetWindowLongPtrW(h, GWLP_USERDATA, 0);
                    delete handler;
                    break;
                default:
                    break;
            }
            return ::DefWindowProcW(h, m, w, l);
        }

        void ensure_class() {
            static bool registered{false};
            if(registered) return;
            WNDCLASSEXW wc{sizeof(wc)};
            wc.lpfnWndProc = window_proc;
            wc.hInstance = ::GetModuleHandleW(nullptr);
            wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = ::GetSysColorBrush(COLOR_WINDOW);
            wc.hIcon = ::LoadIconW(wc.hInstance, L"IDI_ICON1");
            wc.lpszClassName = CLASS_NAME;
            ::RegisterClassExW(&wc);
            registered = true;
        }
    }

    HWND create_window(DWORD ex_style, DWORD style, const string& title, HWND parent,
        int x, int y, int w, int h, msg_handler handler, HMENU menu) {
        ensure_class();
        auto* heap = new msg_handler{std::move(handler)};
        HWND hwnd = ::CreateWindowExW(ex_style, CLASS_NAME, wide(title).c_str(), style, x, y, w, h, parent, menu,
            ::GetModuleHandleW(nullptr), heap);
        if(!hwnd) delete heap;
        return hwnd;
    }

    void form::begin(int x, int top, int w, int label_width, UINT d) {
        clear();
        dpi = d;
        x0 = x;
        y = top;
        width = w;
        label_w = label_width;
        row_h = scale(32, dpi);
        gap = scale(5, dpi);
        last_right = x0;
        last_top = y;
    }

    void form::clear() {
        for(HWND h : controls) {
            ::RemovePropW(h, DIM_PROP);
            ::DestroyWindow(h);
        }
        controls.clear();
        handlers.clear();
        boxes.clear();
    }

    void form::paint(HDC hdc) const {
        Gdiplus::Graphics g{hdc};
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen border{Gdiplus::Color{255, 211, 211, 211}, 1.0f};
        Gdiplus::SolidBrush fill{Gdiplus::Color{255, 255, 255, 255}}, fill_ro{Gdiplus::Color{255, 249, 249, 249}};
        const float r = static_cast<float>(scale(5, dpi));
        for(const auto& [rc, ro] : boxes) {
            Gdiplus::GraphicsPath path;
            const float x = rc.left + 0.5f, y = rc.top + 0.5f, w = static_cast<float>(rc.right - rc.left - 1), h = static_cast<float>(rc.bottom - rc.top - 1);
            const float d = r * 2;
            path.AddArc(x, y, d, d, 180, 90);
            path.AddArc(x + w - d, y, d, d, 270, 90);
            path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
            path.AddArc(x, y + h - d, d, d, 90, 90);
            path.CloseFigure();
            g.FillPath(ro ? &fill_ro : &fill, &path);
            g.DrawPath(&border, &path);
        }
    }

    void form::next_row(int h) {
        last_top = y;
        y += (h ? h : row_h) + gap;
    }

    HWND form::make_label(const string& text, int x, int top, int w, int h, bool dim) {
        HWND l = child(parent, L"Static", text, SS_LEFT | SS_NOPREFIX | (h <= row_h ? SS_CENTERIMAGE : 0), 0, -1, dpi);
        ::MoveWindow(l, x, top, w, h, FALSE);
        if(dim) ::SetPropW(l, DIM_PROP, reinterpret_cast<HANDLE>(1));
        controls.push_back(l);
        return l;
    }

    void form::section(const string& title) {
        if(y > last_top + gap) y += gap;
        HWND l = make_label(title, x0, y, width, row_h);
        ::SendMessageW(l, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font(dpi, 1, true)), FALSE);
        next_row();
    }

    void form::label(const string& text, bool dim, int lines) {
        int h = lines > 1 ? MulDiv(row_h, lines * 3, 4) : row_h;
        make_label(text, x0, y, width, h, dim);
        next_row(h);
    }

    void form::check(const string& text, bool& value, function<void()> changed) {
        bool* v = &value;
        check(text, [v] { return *v; }, [v, changed](bool b) {
            *v = b;
            if(changed) changed();
        });
    }

    void form::check(const string& text, function<bool()> get, function<void(bool)> set) {
        int id = new_id();
        HWND h = child(parent, L"Button", text, BS_AUTOCHECKBOX | WS_TABSTOP, 0, id, dpi);
        ::MoveWindow(h, x0, y, width, row_h, FALSE);
        ::SendMessageW(h, BM_SETCHECK, get() ? BST_CHECKED : BST_UNCHECKED, 0);
        controls.push_back(h);
        handlers[id] = [h, set](int code) {
            if(code == BN_CLICKED) set(::SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED);
        };
        next_row();
    }

    HWND form::text(const string& lbl, string& value, bool read_only, function<void()> changed, int reserve) {
        int id = new_id();
        int cx = x0;
        if(!lbl.empty()) {
            make_label(lbl, x0, y, label_w, row_h);
            cx += label_w;
        }
        // a borderless one-line edit centred in a rounded box that paint() draws
        HWND h = child(parent, L"Edit", value, ES_AUTOHSCROLL | WS_TABSTOP | (read_only ? ES_READONLY : 0), 0, id, dpi);
        const int right = x0 + width - (reserve ? scale(reserve, dpi) : 0);
        boxes.push_back({{cx, y, right, y + row_h}, read_only});
        TEXTMETRICW tm{};
        HDC hdc = ::GetDC(h);
        HGDIOBJ old = ::SelectObject(hdc, ui_font(dpi));
        ::GetTextMetricsW(hdc, &tm);
        ::SelectObject(hdc, old);
        ::ReleaseDC(h, hdc);
        const int inset = scale(8, dpi);
        ::MoveWindow(h, cx + inset, y + (row_h - tm.tmHeight) / 2, right - cx - inset * 2, tm.tmHeight, FALSE);
        controls.push_back(h);
        string* v = &value;
        handlers[id] = [this, h, v, changed](int code) {
            if(code != EN_CHANGE || updating) return;
            *v = get_text(h);
            if(changed) changed();
        };
        last_right = right;
        next_row();
        return h;
    }

    void form::number(const string& lbl, int& value, int lo, int hi, function<void()> changed) {
        int id = new_id();
        make_label(lbl, x0, y, label_w, row_h);
        int w = scale(90, dpi);
        HWND e = child(parent, L"Edit", to_string(value), ES_NUMBER | WS_TABSTOP, WS_EX_CLIENTEDGE, id, dpi);
        ::MoveWindow(e, x0 + label_w, y, w, row_h, FALSE);
        HWND ud = child(parent, UPDOWN_CLASSW, "", UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS, 0, -1, dpi);
        ::SendMessageW(ud, UDM_SETBUDDY, reinterpret_cast<WPARAM>(e), 0);
        ::SendMessageW(ud, UDM_SETRANGE32, lo, hi);
        ::SendMessageW(ud, UDM_SETPOS32, 0, value);
        controls.push_back(e);
        controls.push_back(ud);
        int* v = &value;
        handlers[id] = [e, v, lo, hi, changed](int code) {
            if(code != EN_CHANGE) return;
            try {
                int n = stoi(get_text(e));
                if(n < lo || n > hi) return;
                *v = n;
                if(changed) changed();
            } catch(...) {
            }
        };
        next_row();
    }

    void form::number(const string& lbl, float& value, float lo, float hi, function<void()> changed) {
        int id = new_id();
        make_label(lbl, x0, y, label_w, row_h);
        HWND e = child(parent, L"Edit", format("{:g}", value), WS_TABSTOP, WS_EX_CLIENTEDGE, id, dpi);
        ::MoveWindow(e, x0 + label_w, y, scale(90, dpi), row_h, FALSE);
        controls.push_back(e);
        make_label(format("{:g} – {:g}", lo, hi), x0 + label_w + scale(98, dpi), y, scale(120, dpi), row_h, true);
        float* v = &value;
        handlers[id] = [e, v, lo, hi, changed](int code) {
            if(code != EN_CHANGE) return;
            try {
                float n = stof(get_text(e));
                if(n < lo || n > hi) return;
                *v = n;
                if(changed) changed();
            } catch(...) {
            }
        };
        next_row();
    }

    HWND form::combo(const string& lbl, const vector<string>& items, function<int()> get, function<void(int)> set) {
        int id = new_id();
        int cx = x0;
        if(!lbl.empty()) {
            make_label(lbl, x0, y, label_w, row_h);
            cx += label_w;
        }
        HWND h = child(parent, L"ComboBox", "", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 0, id, dpi);
        RECT wr{};
        ::GetWindowRect(h, &wr);
        const int closed_h = wr.bottom - wr.top;
        ::MoveWindow(h, cx, y + (row_h - closed_h) / 2, min(scale(260, dpi), x0 + width - cx), scale(300, dpi), FALSE);
        for(auto& s : items) ::SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(s).c_str()));
        ::SendMessageW(h, CB_SETCURSEL, get(), 0);
        controls.push_back(h);
        handlers[id] = [h, set](int code) {
            if(code == CBN_SELCHANGE) set(static_cast<int>(::SendMessageW(h, CB_GETCURSEL, 0, 0)));
        };
        next_row();
        return h;
    }

    HWND form::button(const string& text, function<void()> click, bool same_line, int w, bool enabled) {
        int id = new_id();
        HWND h = child(parent, L"Button", text, BS_PUSHBUTTON | WS_TABSTOP, 0, id, dpi);
        if(!w) {
            HDC hdc = ::GetDC(h);
            HGDIOBJ old = ::SelectObject(hdc, ui_font(dpi));
            SIZE sz{};
            wstring wt = wide(text);
            ::GetTextExtentPoint32W(hdc, wt.c_str(), static_cast<int>(wt.size()), &sz);
            ::SelectObject(hdc, old);
            ::ReleaseDC(h, hdc);
            w = max(static_cast<int>(sz.cx) + scale(24, dpi), scale(80, dpi));
        }
        int bx, by;
        if(same_line) {
            bx = last_right + scale(6, dpi);
            by = last_top;
        } else {
            bx = x0;
            by = y;
            next_row();
        }
        ::MoveWindow(h, bx, by, w, row_h + scale(2, dpi), FALSE);
        ::EnableWindow(h, enabled);
        last_right = bx + w;
        controls.push_back(h);
        handlers[id] = [click](int code) {
            if(code == BN_CLICKED && click) click();
        };
        return h;
    }

    void form::spacer(int px) {
        y += scale(px, dpi);
    }

    RECT form::area(int height) {
        RECT r{x0, y, x0 + width, y + height};
        next_row(height);
        return r;
    }

    bool form::on_command(WPARAM w, LPARAM) {
        auto it = handlers.find(LOWORD(w));
        if(it == handlers.end()) return false;
        auto fn = it->second;   // the handler may rebuild the form
        fn(HIWORD(w));
        return true;
    }
}
