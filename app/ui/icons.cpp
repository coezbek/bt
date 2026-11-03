#include "icons.h"
#include <map>
#include <memory>
#include <format>
#include <algorithm>
#include <cwctype>
#include <shlwapi.h>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#include "win32ui.h"
#include "globals.h"
#include "../../res.h"
#include "common/fss.h"
#include "common/str.h"

using namespace std;
using namespace Gdiplus;
using namespace grey::common;

namespace bt::ui::icons {

    static ULONG_PTR gdiplus_token{0};

    // decoded source images in straight ARGB, keyed by path
    static map<string, unique_ptr<Bitmap>> sources;
    // finished premultiplied DIBs, keyed by path or profile identity + size + mode
    static map<string, HBITMAP> dibs;

    void init() {
        if(gdiplus_token) return;
        GdiplusStartupInput input;
        GdiplusStartup(&gdiplus_token, &input, nullptr);
    }

    void clear() {
        for(auto& [k, h] : dibs) ::DeleteObject(h);
        dibs.clear();
        sources.clear();
    }

    void shutdown() {
        clear();
        if(gdiplus_token) GdiplusShutdown(gdiplus_token);
        gdiplus_token = 0;
    }

    static unique_ptr<Bitmap> to_argb(Bitmap* src) {
        if(!src || src->GetLastStatus() != Ok || src->GetWidth() == 0) return nullptr;
        auto r = make_unique<Bitmap>(src->GetWidth(), src->GetHeight(), PixelFormat32bppARGB);
        Graphics g{r.get()};
        g.DrawImage(src, 0, 0, src->GetWidth(), src->GetHeight());
        return r;
    }

    static unique_ptr<Bitmap> from_memory(const unsigned char* data, unsigned int len) {
        IStream* stream = ::SHCreateMemStream(data, len);
        if(!stream) return nullptr;
        unique_ptr<Bitmap> raw{Bitmap::FromStream(stream)};
        auto r = to_argb(raw.get());
        stream->Release();
        return r;
    }

    /**
     * @brief HICON to straight-alpha ARGB. Bitmap::FromHICON drops the alpha channel, so read the bits ourselves.
     */
    static unique_ptr<Bitmap> from_hicon(HICON icon) {
        ICONINFO ii{};
        if(!::GetIconInfo(icon, &ii)) return nullptr;
        BITMAP bm{};
        ::GetObjectW(ii.hbmColor ? ii.hbmColor : ii.hbmMask, sizeof(bm), &bm);
        int w = bm.bmWidth, h = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;
        unique_ptr<Bitmap> r;

        if(ii.hbmColor && w > 0 && h > 0) {
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = w;
            bi.bmiHeader.biHeight = -h;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            vector<uint32_t> px(static_cast<size_t>(w) * h);
            HDC hdc = ::GetDC(nullptr);
            ::GetDIBits(hdc, ii.hbmColor, 0, h, px.data(), &bi, DIB_RGB_COLORS);

            bool has_alpha = any_of(px.begin(), px.end(), [](uint32_t v) { return (v >> 24) != 0; });
            if(!has_alpha) {
                // old style icon: transparency lives in the AND mask
                vector<uint32_t> mask(px.size());
                ::GetDIBits(hdc, ii.hbmMask, 0, h, mask.data(), &bi, DIB_RGB_COLORS);
                for(size_t i = 0; i < px.size(); i++) {
                    px[i] = (px[i] & 0x00FFFFFF) | ((mask[i] & 0x00FFFFFF) ? 0 : 0xFF000000);
                }
            }
            ::ReleaseDC(nullptr, hdc);

            r = make_unique<Bitmap>(w, h, PixelFormat32bppARGB);
            BitmapData data{};
            Rect rc{0, 0, w, h};
            r->LockBits(&rc, ImageLockModeWrite, PixelFormat32bppARGB, &data);
            for(int y = 0; y < h; y++) {
                memcpy(static_cast<BYTE*>(data.Scan0) + y * data.Stride, &px[static_cast<size_t>(y) * w], w * 4);
            }
            r->UnlockBits(&data);
        }

        if(ii.hbmColor) ::DeleteObject(ii.hbmColor);
        if(ii.hbmMask) ::DeleteObject(ii.hbmMask);
        return r;
    }

    static unique_ptr<Bitmap> from_icon_file(const wstring& path, int index) {
        HICON icon{nullptr};
        UINT id{0};
        if(::PrivateExtractIconsW(path.c_str(), index, 256, 256, &icon, &id, 1, 0) != 1 || !icon) return nullptr;
        auto r = from_hicon(icon);
        ::DestroyIcon(icon);
        return r;
    }

    static unique_ptr<Bitmap> load(const string& key) {
        if(key == "res:black_hole") return from_memory(black_hole_png, black_hole_png_len);
        if(key == "incognito") return from_memory(incognito_icon_png, incognito_icon_png_len);
        if(key == "logo") return from_memory(icon_png, icon_png_len);
        if(key == "bt_chromium") return from_memory(chromium_icon_png, chromium_icon_png_len);
        if(key == "bt_gecko") return from_memory(gecko64_icon_png, gecko64_icon_png_len);
        if(key.empty() || key.starts_with("md_") || key.starts_with("res:")) return nullptr;

        string path = key;
        str::trim(path);
        str::trim(path, "\"");

        // "file.exe,3" selects an icon by index
        int index{0};
        if(size_t comma = path.rfind(','); comma != string::npos && comma > path.rfind('\\')) {
            try {
                index = stoi(path.substr(comma + 1));
                path = path.substr(0, comma);
            } catch(...) {
            }
        }

        path = fss::get_full_path(path);
        const wstring wpath = wide(path);
        if(::GetFileAttributesW(wpath.c_str()) == INVALID_FILE_ATTRIBUTES) return nullptr;

        string lower = path;
        ranges::transform(lower, lower.begin(), ::tolower);
        if(lower.ends_with(".exe") || lower.ends_with(".dll") || lower.ends_with(".ico")) {
            return from_icon_file(wpath, index);
        }

        unique_ptr<Bitmap> raw{Bitmap::FromFile(wpath.c_str())};
        return to_argb(raw.get());
    }

    static Bitmap* source(const string& key) {
        auto it = sources.find(key);
        if(it == sources.end()) it = sources.emplace(key, load(key)).first;
        return it->second.get();
    }

    static void fill_circle(Graphics& g, Color c, float x, float y, float d) {
        SolidBrush brush{c};
        g.FillEllipse(&brush, x, y, d, d);
    }

    /**
     * @brief Draws an image clipped to a circle. Without an image, draws a grey disc with the fallback letter.
     */
    static void draw_round(Graphics& g, Bitmap* img, float x, float y, float d, const string& fallback_letter, bool circle) {
        if(img) {
            if(circle) {
                GraphicsPath path;
                path.AddEllipse(x, y, d, d);
                g.SetClip(&path);
                g.DrawImage(img, x, y, d, d);
                g.ResetClip();
            } else {
                g.DrawImage(img, x, y, d, d);
            }
            return;
        }

        fill_circle(g, Color{255, 120, 120, 128}, x, y, d);
        if(!fallback_letter.empty()) {
            FontFamily ff{L"Segoe UI"};
            Font font{&ff, d * 0.5f, FontStyleBold, UnitPixel};
            StringFormat sf;
            sf.SetAlignment(StringAlignmentCenter);
            sf.SetLineAlignment(StringAlignmentCenter);
            SolidBrush white{Color{255, 255, 255, 255}};
            wstring w = wide(fallback_letter);
            g.DrawString(w.c_str(), static_cast<INT>(w.size()), &font, RectF{x, y, d, d}, &sf, &white);
        }
    }

    static string first_letter(const string& s) {
        if(s.empty()) return {};
        wstring w = wide(s);
        return narrow(wstring(1, static_cast<wchar_t>(towupper(w[0]))));
    }

    /**
     * @brief Works out which images a profile shows, like the ImGui version did.
     */
    static void pick_paths(const browser& b, const browser_profile& p, icon_overlay_mode mode, string& icon1, string& icon2) {
        icon1 = b.get_best_icon_path();
        icon2.clear();
        if(b.engine != browser_engine::generic) {
            if(p.is_incognito && p.user_icon_path.empty())
                icon2 = "incognito";
            else
                icon2 = b.get_best_icon_path(p);
        } else if(!p.user_icon_path.empty()) {
            icon1 = p.user_icon_path;
        }

        switch(mode) {
            case icon_overlay_mode::browser_only:
                icon2.clear();
                break;
            case icon_overlay_mode::profile_only:
                if(!icon2.empty()) icon1 = icon2;
                icon2.clear();
                break;
            case icon_overlay_mode::browser_on_profile:
                if(!icon2.empty()) swap(icon1, icon2);
                break;
            case icon_overlay_mode::profile_on_browser:
                break;
        }
        if(icon1 == icon2) icon2.clear();
    }

    static unique_ptr<Bitmap> compose_profile(const browser& b, const browser_profile& p, int px, icon_overlay_mode mode) {
        string icon1, icon2;
        pick_paths(b, p, mode, icon1, icon2);

        auto canvas = make_unique<Bitmap>(px, px, PixelFormat32bppARGB);
        Graphics g{canvas.get()};
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.Clear(Color{0, 0, 0, 0});

        float inset{0};
        if(p.use_color || p.use_user_color) {
            const grey::rgb_colour& c = p.use_user_color ? p.user_color : p.color;
            float ring = max(1.0f, static_cast<float>(g_state.highlight_width) * px / 48.0f);
            fill_circle(g, Color{255, c.r8(), c.g8(), c.b8()}, 0, 0, static_cast<float>(px));
            inset = ring;
        }

        float d = px - inset * 2;
        draw_round(g, source(icon1), inset, inset, d, first_letter(b.engine == browser_engine::generic ? b.name : p.name), true);

        if(!icon2.empty()) {
            float od = d / 2;
            Bitmap* img2 = source(icon2);
            draw_round(g, img2, inset + d - od, inset + d - od, od, img2 ? "" : first_letter(p.name), true);
        }
        return canvas;
    }

    static HBITMAP to_premultiplied_dib(Bitmap* bmp) {
        int w = static_cast<int>(bmp->GetWidth()), h = static_cast<int>(bmp->GetHeight());
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits{nullptr};
        HBITMAP dib = ::CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if(!dib) return nullptr;

        BitmapData data{};
        Rect rc{0, 0, w, h};
        bmp->LockBits(&rc, ImageLockModeRead, PixelFormat32bppPARGB, &data);
        for(int y = 0; y < h; y++) {
            memcpy(static_cast<BYTE*>(bits) + static_cast<size_t>(y) * w * 4,
                static_cast<BYTE*>(data.Scan0) + y * data.Stride, static_cast<size_t>(w) * 4);
        }
        bmp->UnlockBits(&data);
        return dib;
    }

    static string profile_key(const browser& b, const browser_profile& p, int px, icon_overlay_mode mode) {
        return format("p|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}", b.get_best_icon_path(), b.get_best_icon_path(p),
            p.user_icon_path, p.is_incognito, static_cast<int>(b.engine), b.name, p.name,
            p.use_user_color ? p.user_color.to_hex(false) : "", p.use_color ? p.color.to_hex(false) : "",
            px, static_cast<int>(mode));
    }

    HBITMAP profile_bitmap(const browser& b, const browser_profile& p, int px, icon_overlay_mode mode) {
        string key = profile_key(b, p, px, mode);
        if(auto it = dibs.find(key); it != dibs.end()) return it->second;
        auto canvas = compose_profile(b, p, px, mode);
        HBITMAP dib = to_premultiplied_dib(canvas.get());
        dibs[key] = dib;
        return dib;
    }

    static unique_ptr<Bitmap> compose_path(const string& path, int px, bool circle) {
        auto canvas = make_unique<Bitmap>(px, px, PixelFormat32bppARGB);
        Graphics g{canvas.get()};
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.Clear(Color{0, 0, 0, 0});
        Bitmap* img = source(path);
        if(!img) img = source("logo");
        draw_round(g, img, 0, 0, static_cast<float>(px), "", circle);
        return canvas;
    }

    HBITMAP path_bitmap(const string& path, int px, bool circle) {
        string key = format("f|{}|{}|{}", path, px, circle);
        if(auto it = dibs.find(key); it != dibs.end()) return it->second;
        auto canvas = compose_path(path, px, circle);
        HBITMAP dib = to_premultiplied_dib(canvas.get());
        dibs[key] = dib;
        return dib;
    }

    static HICON to_hicon(Bitmap* bmp) {
        HICON icon{nullptr};
        bmp->GetHICON(&icon);
        return icon;
    }

    HICON profile_icon(const browser& b, const browser_profile& p, int px, icon_overlay_mode mode) {
        auto canvas = compose_profile(b, p, px, mode);
        return to_hicon(canvas.get());
    }

    HICON path_icon(const string& path, int px) {
        auto canvas = compose_path(path, px, false);
        return to_hicon(canvas.get());
    }

    void draw(HDC hdc, HBITMAP bmp, int x, int y) {
        if(!bmp) return;
        BITMAP bm{};
        ::GetObjectW(bmp, sizeof(bm), &bm);
        HDC mem = ::CreateCompatibleDC(hdc);
        HGDIOBJ old = ::SelectObject(mem, bmp);
        BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        ::AlphaBlend(hdc, x, y, bm.bmWidth, bm.bmHeight, mem, 0, 0, bm.bmWidth, bm.bmHeight, bf);
        ::SelectObject(mem, old);
        ::DeleteDC(mem);
    }

    void warm(int px, icon_overlay_mode mode) {
        for(const browser& b : g_state.browsers) {
            if(b.is_hidden) continue;
            for(const browser_profile& p : b.profiles) {
                if(p.is_hidden) continue;
                profile_bitmap(b, p, px, mode);
            }
        }
    }
}
