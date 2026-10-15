#pragma once
// Shared Win32 presentation kit for the Lantern client and host manager.
// Design tokens match the Android client: one cool neutral base, ink for primary actions and a single
// amber "lamp" that only means live (connected, streaming, controlling, running). Everything is sized
// in device-independent units through dp() and follows the system light/dark setting.
// Presentation only: no protocol, credential or file logic lives here.
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <objidl.h>
namespace Gdiplus {
using std::max;
using std::min;
} // namespace Gdiplus
#include <gdiplus.h>
#include "app_icon.h"
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")

namespace lantern::ui {

// ---------------------------------------------------------------- tokens
struct Palette {
    COLORREF bg, rail, surface, inset, line, ink, muted, faint, on_ink, lamp, lamp_text, positive, danger,
        chrome, chrome_ink;
    COLORREF lamp_soft, danger_soft, hover;
};
inline Palette pal{};
inline bool dark = false;
inline UINT dpi = 96;
inline int dp(double v) {
    return int(std::lround(v * dpi / 96.0));
}
inline COLORREF mix(COLORREF a, COLORREF b, double t) {
    auto c = [t](int x, int y) { return BYTE(std::lround(x + (y - x) * t)); };
    return RGB(c(GetRValue(a), GetRValue(b)), c(GetGValue(a), GetGValue(b)), c(GetBValue(a), GetBValue(b)));
}
inline bool system_dark() {
    DWORD value = 1, size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value == 0;
}
inline std::map<COLORREF, HBRUSH> brushes;
inline HBRUSH brush(COLORREF c) {
    auto& b = brushes[c];
    if (!b)
        b = CreateSolidBrush(c);
    return b;
}
inline void free_brushes() {
    for (auto& [c, b] : brushes)
        DeleteObject(b);
    brushes.clear();
}
inline void load_palette() {
    dark = system_dark();
    if (dark)
        pal = {RGB(14, 19, 25),    RGB(18, 24, 31),    RGB(22, 28, 36),   RGB(31, 39, 50),
               RGB(39, 49, 61),    RGB(233, 237, 242), RGB(151, 161, 174), RGB(102, 113, 126),
               RGB(16, 21, 27),    RGB(246, 173, 60),  RGB(247, 187, 92), RGB(76, 193, 150),
               RGB(255, 107, 97),  RGB(10, 13, 17),    RGB(233, 237, 242)};
    else
        pal = {RGB(238, 241, 244), RGB(229, 233, 238), RGB(255, 255, 255), RGB(237, 240, 244),
               RGB(221, 226, 232), RGB(27, 36, 48),    RGB(102, 113, 128), RGB(154, 163, 174),
               RGB(255, 255, 255), RGB(243, 162, 35),  RGB(168, 100, 0),   RGB(35, 132, 95),
               RGB(207, 61, 53),   RGB(14, 18, 24),    RGB(233, 237, 242)};
    pal.lamp_soft = mix(pal.surface, pal.lamp, dark ? .22 : .17);
    pal.danger_soft = mix(pal.surface, pal.danger, dark ? .16 : .10);
    pal.hover = mix(pal.inset, pal.ink, dark ? .08 : .05);
    free_brushes();
}

// ---------------------------------------------------------------- fonts
struct Fonts {
    HFONT body{}, note{}, strong{}, heading{}, title{}, icon{}, icon_small{}, icon_large{}, mono{};
};
inline Fonts fonts;
inline std::wstring icon_face = L"Segoe MDL2 Assets";
inline bool has_font(const wchar_t* face) {
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(lf.lfFaceName, face, _TRUNCATE);
    bool found = false;
    HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(
        dc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int {
            *reinterpret_cast<bool*>(p) = true;
            return 0;
        },
        reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, dc);
    return found;
}
inline HFONT make_font(int px, int weight, const wchar_t* face) {
    return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}
inline void free_fonts() {
    for (HFONT f : {fonts.body, fonts.note, fonts.strong, fonts.heading, fonts.title, fonts.icon,
                    fonts.icon_small, fonts.icon_large, fonts.mono})
        if (f)
            DeleteObject(f);
    fonts = {};
}
inline void make_fonts() {
    free_fonts();
    const wchar_t* face = L"Microsoft YaHei UI";
    if (has_font(L"Segoe Fluent Icons"))
        icon_face = L"Segoe Fluent Icons";
    fonts.body = make_font(dp(14), FW_NORMAL, face);
    fonts.note = make_font(dp(12.5), FW_NORMAL, face);
    fonts.strong = make_font(dp(14), FW_SEMIBOLD, face);
    fonts.heading = make_font(dp(16), FW_SEMIBOLD, face);
    fonts.title = make_font(dp(24), FW_SEMIBOLD, face);
    fonts.icon = make_font(dp(16), FW_NORMAL, icon_face.c_str());
    fonts.icon_small = make_font(dp(12), FW_NORMAL, icon_face.c_str());
    fonts.icon_large = make_font(dp(34), FW_NORMAL, icon_face.c_str());
    fonts.mono = make_font(dp(13), FW_NORMAL, L"Consolas");
}

// Glyphs shared by Segoe MDL2 Assets and Segoe Fluent Icons.
namespace glyph {
constexpr wchar_t connect = 0xE703, desktop = 0xE7F4, folder = 0xE8B7, transfer = 0xE8AB, refresh = 0xE72C,
                  up = 0xE74A, search = 0xE721, filter = 0xE71C, upload = 0xE898, download = 0xE896,
                  more = 0xE712, open = 0xE8E5, sort = 0xE8CB, fullscreen = 0xE740, exit_full = 0xE73F,
                  keyboard = 0xE765, paste = 0xE77F, copy = 0xE8C8, speaker = 0xE767, mute = 0xE74F,
                  hide = 0xED1A, view = 0xE890, pointer = 0xE962, play = 0xE768, pause = 0xE769,
                  remove = 0xE74D, cancel = 0xE711, rename = 0xE8AC, add = 0xE710, info = 0xE946,
                  settings = 0xE713, diagnose = 0xE9D9, shield = 0xEA18, clock = 0xE823, chevron_up = 0xE70E,
                  chevron_down = 0xE70D, check = 0xE73E, back = 0xE72B, sync = 0xE895, monitors = 0xE7F4,
                  zoom = 0xE71E, people = 0xE716, plug = 0xE839, key = 0xE192, save = 0xE74E, stop = 0xE71A,
                  power = 0xE7E8, home = 0xE80F, save_as = 0xE792, link = 0xE71B, video = 0xE714;
} // namespace glyph

// ---------------------------------------------------------------- drawing
inline Gdiplus::Color gcolor(COLORREF c, BYTE a = 255) {
    return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}
inline void round_rect(HDC dc, RECT r, int radius, COLORREF fill, COLORREF border = CLR_INVALID, int width = 1) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    float x = float(r.left), y = float(r.top), w = float(r.right - r.left), h = float(r.bottom - r.top);
    if (border != CLR_INVALID) {
        float in = width / 2.f;
        x += in, y += in, w -= width, h -= width;
    }
    if (w <= 0 || h <= 0)
        return;
    float d = std::min({radius * 2.f, w, h});
    Gdiplus::GraphicsPath p;
    if (d < 1)
        p.AddRectangle(Gdiplus::RectF(x, y, w, h));
    else {
        p.AddArc(x, y, d, d, 180, 90);
        p.AddArc(x + w - d, y, d, d, 270, 90);
        p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
        p.AddArc(x, y + h - d, d, d, 90, 90);
        p.CloseFigure();
    }
    if (fill != CLR_INVALID) {
        Gdiplus::SolidBrush b(gcolor(fill));
        g.FillPath(&b, &p);
    }
    if (border != CLR_INVALID) {
        Gdiplus::Pen pen(gcolor(border), float(width));
        g.DrawPath(&pen, &p);
    }
}
inline void circle(HDC dc, int cx, int cy, int r, COLORREF c) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush b(gcolor(c));
    g.FillEllipse(&b, float(cx - r), float(cy - r), float(2 * r), float(2 * r));
}
inline void draw_text(HDC dc, const std::wstring& s, RECT r, HFONT f, COLORREF c,
                      UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX) {
    auto old = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    DrawTextW(dc, s.c_str(), int(s.size()), &r, flags);
    SelectObject(dc, old);
}
inline int text_width(HFONT f, const std::wstring& s) {
    HDC dc = GetDC(nullptr);
    auto old = SelectObject(dc, f);
    SIZE z{};
    GetTextExtentPoint32W(dc, s.c_str(), int(s.size()), &z);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    return z.cx;
}
inline int font_height(HFONT f) {
    HDC dc = GetDC(nullptr);
    auto old = SelectObject(dc, f);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    return tm.tmHeight;
}
inline std::wstring window_text(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n + 1, 0);
    GetWindowTextW(h, s.data(), n + 1);
    s.resize(n);
    return s;
}

// ---------------------------------------------------------------- owner-drawn buttons
enum class Kind { Primary, Secondary, Ghost, Danger, Nav, Tile, Chip, Icon };
/** What the control sits on; decides fills so rounded corners blend in. */
enum class Surface { Bg, Card, Rail, Chrome };
struct Item {
    Kind kind = Kind::Secondary;
    wchar_t glyph = 0;
    Surface surface = Surface::Bg;
    std::function<bool()> on;          // live state for Nav / Tile
    std::wstring on_text, off_text;    // Tile second line
    bool hover = false, checked = false;
};
inline std::map<HWND, Item> items;
inline COLORREF surface_color(Surface s) {
    switch (s) {
    case Surface::Card: return pal.surface;
    case Surface::Rail: return pal.rail;
    case Surface::Chrome: return pal.chrome;
    default: return pal.bg;
    }
}
inline LRESULT CALLBACK button_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    auto it = items.find(h);
    if (it != items.end()) {
        switch (m) {
        case WM_MOUSEMOVE:
            if (!it->second.hover) {
                it->second.hover = true;
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
                TrackMouseEvent(&t);
                InvalidateRect(h, nullptr, FALSE);
            }
            break;
        case WM_MOUSELEAVE:
            it->second.hover = false;
            InvalidateRect(h, nullptr, FALSE);
            break;
        case BM_SETCHECK:
            it->second.checked = w == BST_CHECKED;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case BM_GETCHECK:
            return it->second.checked ? BST_CHECKED : BST_UNCHECKED;
        case WM_ERASEBKGND:
            return 1;
        case WM_NCDESTROY:
            items.erase(it);
            RemoveWindowSubclass(h, button_proc, 1);
            break;
        }
    }
    return DefSubclassProc(h, m, w, l);
}
inline HWND button(HWND parent, int id, Kind kind, const wchar_t* label, wchar_t g = 0,
                   Surface surface = Surface::Bg) {
    auto h = CreateWindowExW(0, L"BUTTON", label, WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, parent,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    Item item;
    item.kind = kind;
    item.glyph = g;
    item.surface = surface;
    items[h] = std::move(item);
    SetWindowSubclass(h, button_proc, 1, 0);
    SendMessageW(h, WM_SETFONT, (WPARAM)fonts.body, FALSE);
    return h;
}
inline Item* item(HWND h) {
    auto it = items.find(h);
    return it == items.end() ? nullptr : &it->second;
}
inline void set_surface(HWND h, Surface s) {
    if (auto i = item(h); i && i->surface != s) {
        i->surface = s;
        InvalidateRect(h, nullptr, FALSE);
    }
}
/** Chips behave like check boxes: the click toggles before the owner sees BN_CLICKED. */
inline void clicked(HWND h) {
    if (auto i = item(h); i && i->kind == Kind::Chip) {
        i->checked = !i->checked;
        InvalidateRect(h, nullptr, FALSE);
    }
}
inline int preferred_width(HWND h, int height) {
    auto i = item(h);
    if (!i)
        return dp(100);
    auto label = window_text(h);
    if (i->kind == Kind::Icon)
        return height;
    bool tile = i->kind == Kind::Tile && height >= dp(46);
    int w = text_width(tile ? fonts.strong : fonts.body, label);
    if (tile)
        w = std::max({w, text_width(fonts.note, i->on_text), text_width(fonts.note, i->off_text)});
    if (i->kind == Kind::Chip)
        w += dp(22);
    if (i->glyph)
        w += dp(tile ? 30 : 24);
    return w + dp(i->kind == Kind::Tile ? 30 : 32);
}
inline void draw(const DRAWITEMSTRUCT* d) {
    auto i = item(d->hwndItem);
    if (!i)
        return;
    RECT r = d->rcItem;
    int w = r.right - r.left, h = r.bottom - r.top;
    HDC dc = CreateCompatibleDC(d->hDC);
    HBITMAP bmp = CreateCompatibleBitmap(d->hDC, w, h);
    auto oldbmp = SelectObject(dc, bmp);
    COLORREF under = surface_color(i->surface);
    RECT all{0, 0, w, h};
    FillRect(dc, &all, brush(under));
    bool disabled = d->itemState & ODS_DISABLED, pressed = d->itemState & ODS_SELECTED,
         focus = (d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT),
         on = i->kind == Kind::Chip ? i->checked : (i->on && i->on()), hover = i->hover && !disabled;
    bool chrome = i->surface == Surface::Chrome;
    COLORREF raised = i->surface == Surface::Card ? pal.inset : pal.surface;
    COLORREF fill = CLR_INVALID, border = CLR_INVALID, fg = chrome ? pal.chrome_ink : pal.ink, gl = fg,
             sub = chrome ? mix(pal.chrome_ink, pal.chrome, .35) : pal.muted;
    int radius = dp(8);
    auto lift = [&](COLORREF c) { return mix(c, chrome ? RGB(255, 255, 255) : pal.ink, pressed ? .12 : hover ? .06 : 0); };
    switch (i->kind) {
    case Kind::Primary:
        fill = mix(pal.ink, under, pressed ? .25 : hover ? .14 : 0);
        fg = gl = pal.on_ink;
        if (disabled)
            fill = mix(pal.ink, under, .65);
        break;
    case Kind::Secondary:
        fill = lift(chrome ? mix(pal.chrome, RGB(255, 255, 255), .10) : raised);
        if (i->surface == Surface::Bg)
            border = pal.line;
        break;
    case Kind::Danger:
        fill = lift(chrome ? mix(pal.chrome, pal.danger, .25) : pal.danger_soft);
        fg = gl = chrome ? pal.chrome_ink : pal.danger;
        break;
    case Kind::Ghost:
    case Kind::Icon:
        if (hover || pressed)
            fill = lift(under);
        gl = chrome ? pal.chrome_ink : pal.muted;
        if (i->kind == Kind::Icon)
            gl = fg;
        break;
    case Kind::Nav:
        radius = dp(10);
        if (on)
            fill = pal.surface;
        else if (hover || pressed)
            fill = lift(under);
        fg = on ? pal.ink : pal.muted;
        gl = on ? pal.lamp_text : pal.muted;
        break;
    case Kind::Tile:
        radius = dp(12);
        if (chrome)
            fill = on ? mix(pal.chrome, pal.lamp, .30) : lift(mix(pal.chrome, RGB(255, 255, 255), .10));
        else {
            fill = on ? pal.lamp_soft : lift(raised);
            border = on ? pal.lamp : (i->surface == Surface::Bg ? pal.line : CLR_INVALID);
        }
        gl = on ? (chrome ? pal.lamp : pal.lamp_text) : sub;
        if (on && !chrome)
            sub = pal.lamp_text;
        break;
    case Kind::Chip:
        radius = h / 2;
        fill = on ? pal.lamp_soft : (hover ? pal.hover : CLR_INVALID);
        border = on ? pal.lamp : pal.line;
        fg = on ? pal.ink : pal.muted;
        gl = pal.lamp_text;
        break;
    }
    if (disabled && i->kind != Kind::Primary)
        fg = gl = sub = chrome ? mix(pal.chrome_ink, pal.chrome, .55) : pal.faint;
    RECT body{0, 0, w, h};
    if (fill != CLR_INVALID || border != CLR_INVALID)
        round_rect(dc, body, radius, fill, border);
    auto label = window_text(d->hwndItem);
    int gw = i->glyph ? dp(i->kind == Kind::Tile ? 30 : 24) : 0;
    wchar_t gs[2] = {i->glyph, 0};
    if (i->kind == Kind::Icon) {
        draw_text(dc, gs, body, fonts.icon, gl, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else if (i->kind == Kind::Nav) {
        if (on)
            round_rect(dc, RECT{dp(4), h / 2 - dp(8), dp(7), h / 2 + dp(8)}, dp(2), pal.lamp);
        draw_text(dc, gs, RECT{dp(16), 0, dp(16) + gw, h}, fonts.icon, gl, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        draw_text(dc, label, RECT{dp(16) + gw + dp(8), 0, w - dp(8), h}, on ? fonts.strong : fonts.body, fg);
    } else if (i->kind == Kind::Tile && h >= dp(46)) {
        int x = dp(14);
        if (i->glyph) {
            draw_text(dc, gs, RECT{x, 0, x + gw, h}, fonts.icon, gl, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            x += gw;
        }
        int lh = font_height(fonts.strong), sh = font_height(fonts.note), top = (h - lh - sh - dp(2)) / 2;
        draw_text(dc, label, RECT{x, top, w - dp(10), top + lh}, fonts.strong, fg);
        draw_text(dc, on ? i->on_text : i->off_text, RECT{x, top + lh + dp(2), w - dp(10), top + lh + dp(2) + sh},
                  fonts.note, sub);
        if (on)
            circle(dc, w - dp(12), dp(12), dp(3), pal.lamp);
    } else {
        HFONT f = fonts.body;
        int tw = text_width(f, label) + (i->kind == Kind::Chip ? dp(22) : 0);
        int total = tw + gw, x = std::max(dp(8), (w - total) / 2);
        if (i->kind == Kind::Chip) {
            wchar_t mark[2] = {on ? glyph::check : glyph::add, 0};
            draw_text(dc, mark, RECT{x, 0, x + dp(18), h}, fonts.icon_small, on ? pal.lamp_text : pal.faint,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            x += dp(22);
            tw -= dp(22);
        } else if (i->glyph) {
            draw_text(dc, gs, RECT{x, 0, x + gw, h}, fonts.icon, gl, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            x += gw;
        }
        if (i->kind == Kind::Tile && on)
            circle(dc, w - dp(9), h / 2, dp(3), pal.lamp);
        draw_text(dc, label, RECT{x, 0, std::min(w - dp(6), x + tw + dp(2)), h}, f, fg);
    }
    if (focus)
        round_rect(dc, RECT{1, 1, w - 1, h - 1}, std::max(radius - 1, 2), CLR_INVALID, pal.lamp, dp(2));
    BitBlt(d->hDC, r.left, r.top, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldbmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

// ---------------------------------------------------------------- painted scene
enum class Tone { Ink, Muted, Faint, Lamp, Danger, Positive, ChromeInk };
inline COLORREF tone(Tone t) {
    switch (t) {
    case Tone::Muted: return pal.muted;
    case Tone::Faint: return pal.faint;
    case Tone::Lamp: return pal.lamp_text;
    case Tone::Danger: return pal.danger;
    case Tone::Positive: return pal.positive;
    case Tone::ChromeInk: return pal.chrome_ink;
    default: return pal.ink;
    }
}
struct Card {
    RECT r;
    Surface fill = Surface::Card;
    bool outline = true;
};
struct Label {
    RECT r;
    std::wstring s;
    HFONT* font;
    Tone tone;
    UINT flags;
};
struct Dot {
    int x, y, r;
    COLORREF c;
};
struct Field {
    HWND edit;
    RECT outer;
};
struct Scene {
    std::vector<RECT> fills; // full-bleed solid areas (rail, chrome)
    std::vector<COLORREF> fill_colors;
    std::vector<Card> cards;
    std::vector<Label> labels;
    std::vector<Dot> dots;
    std::vector<Field> fields;
    std::vector<RECT> hairlines;
    void clear() { *this = Scene{}; }
    /** Inset well with no edit inside (image preview, etc.). */
    void well(RECT r) { fields.push_back({nullptr, r}); }
    void area(RECT r, COLORREF c) {
        fills.push_back(r);
        fill_colors.push_back(c);
    }
    void card(RECT r, Surface s = Surface::Card, bool outline = true) { cards.push_back({r, s, outline}); }
    void text(RECT r, std::wstring s, HFONT* f, Tone t = Tone::Ink,
              UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX) {
        labels.push_back({r, std::move(s), f, t, flags});
    }
};
/** Inputs inside a card use the inset fill; on the page background they are white with a hairline. */
inline bool on_card(const Scene& s, RECT r) {
    POINT p{r.left + 1, r.top + 1};
    for (auto& c : s.cards)
        if (PtInRect(&c.r, p))
            return true;
    return false;
}
inline COLORREF field_fill(const Scene& s, const Field& f) {
    return !f.edit || on_card(s, f.outer) ? pal.inset : pal.surface;
}
/** Places a borderless edit inside a painted rounded frame; the frame shows focus. */
inline void field(Scene& s, HWND edit, RECT outer) {
    s.fields.push_back({edit, outer});
    int ow = outer.right - outer.left, oh = outer.bottom - outer.top;
    if (GetWindowLongPtrW(edit, GWL_STYLE) & ES_MULTILINE) {
        MoveWindow(edit, outer.left + dp(12), outer.top + dp(8), ow - dp(24), oh - dp(16), TRUE);
        return;
    }
    int fh = font_height((HFONT)SendMessageW(edit, WM_GETFONT, 0, 0)) + dp(2);
    MoveWindow(edit, outer.left + dp(12), outer.top + (oh - fh) / 2, ow - dp(24), fh, TRUE);
}
inline void invalidate_field(HWND window, const Scene& s, HWND edit) {
    for (auto& f : s.fields)
        if (f.edit == edit) {
            RECT r = f.outer;
            InflateRect(&r, 2, 2);
            InvalidateRect(window, &r, FALSE);
        }
}
inline void paint(HWND window, const Scene& s, const std::function<void(HDC)>& extra = {}) {
    PAINTSTRUCT ps;
    HDC target = BeginPaint(window, &ps);
    RECT client{};
    GetClientRect(window, &client);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, std::max(1L, client.right), std::max(1L, client.bottom));
    auto old = SelectObject(dc, bmp);
    FillRect(dc, &client, brush(pal.bg));
    for (size_t i = 0; i < s.fills.size(); ++i)
        FillRect(dc, &s.fills[i], brush(s.fill_colors[i]));
    for (auto& c : s.cards)
        round_rect(dc, c.r, dp(14), surface_color(c.fill), c.outline ? pal.line : CLR_INVALID);
    for (auto& r : s.hairlines)
        FillRect(dc, &r, brush(pal.line));
    HWND focus = GetFocus();
    for (auto& f : s.fields) {
        if (f.edit && !IsWindowVisible(f.edit))
            continue;
        bool focused = f.edit && focus == f.edit;
        COLORREF fill = field_fill(s, f);
        COLORREF edge = focused ? pal.lamp : fill == pal.surface ? pal.line : CLR_INVALID;
        round_rect(dc, f.outer, dp(8), fill, edge, focused ? dp(2) : 1);
    }
    for (auto& l : s.labels)
        draw_text(dc, l.s, l.r, *l.font, tone(l.tone), l.flags);
    for (auto& d : s.dots)
        circle(dc, d.x, d.y, d.r, d.c);
    if (extra)
        extra(dc);
    BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(window, &ps);
}
/** Background colour for a child control: inputs sit on the inset frame, labels on whatever is under. */
inline COLORREF under(const Scene& s, HWND child) {
    for (auto& f : s.fields)
        if (f.edit == child)
            return field_fill(s, f);
    RECT r{};
    GetWindowRect(child, &r);
    MapWindowPoints(nullptr, GetParent(child), reinterpret_cast<POINT*>(&r), 2);
    POINT p{r.left + 1, r.top + 1};
    COLORREF c = pal.bg;
    for (size_t i = 0; i < s.fills.size(); ++i)
        if (PtInRect(&s.fills[i], p))
            c = s.fill_colors[i];
    for (auto& card : s.cards)
        if (PtInRect(&card.r, p))
            c = surface_color(card.fill);
    for (auto& f : s.fields)
        if (!f.edit && PtInRect(&f.outer, p))
            c = pal.inset;
    return c;
}

// ---------------------------------------------------------------- flow layout
/** Left-to-right placement that wraps to a new row instead of clipping. */
struct Flow {
    int left, right, x, y, gap, row = 0;
    Flow(int l, int r, int top, int g) : left(l), right(r), x(l), y(top), gap(g) {}
    RECT next(int w, int h) {
        if (x > left && x + w > right) {
            x = left;
            y += row + gap;
            row = 0;
        }
        RECT r{x, y, x + w, y + h};
        x += w + gap;
        row = std::max(row, h);
        return r;
    }
    void place(HWND c, int h) {
        auto r = next(preferred_width(c, h), h);
        MoveWindow(c, r.left, r.top, r.right - r.left, h, TRUE);
    }
    int bottom() const { return y + row; }
};
/** Width a row of buttons needs at the given height. */
inline int row_width(std::initializer_list<HWND> list, int h, int gap) {
    int w = 0;
    for (auto c : list)
        w += preferred_width(c, h) + gap;
    return list.size() ? w - gap : 0;
}

// ---------------------------------------------------------------- native control styling
inline void style_list(HWND list) {
    SetWindowTheme(list, dark ? L"DarkMode_ItemsView" : L"ItemsView", nullptr);
    if (auto header = ListView_GetHeader(list))
        SetWindowTheme(header, dark ? L"DarkMode_ItemsView" : L"ItemsView", nullptr);
    ListView_SetBkColor(list, pal.surface);
    ListView_SetTextBkColor(list, pal.surface);
    ListView_SetTextColor(list, pal.ink);
    // A one-pixel-wide image list is the standard way to get taller report rows.
    auto old = ListView_SetImageList(list, ImageList_Create(1, dp(34), ILC_COLOR32, 0, 1), LVSIL_SMALL);
    if (old)
        ImageList_Destroy(old);
}
inline void style_combo(HWND combo, bool force_dark = false) {
    SetWindowTheme(combo, dark || force_dark ? L"DarkMode_CFD" : nullptr, nullptr);
    SendMessageW(combo, CB_SETITEMHEIGHT, WPARAM(-1), dp(28));
    SendMessageW(combo, CB_SETITEMHEIGHT, 0, dp(26));
}
inline void style_scroll(HWND edit) {
    SetWindowTheme(edit, dark ? L"DarkMode_Explorer" : nullptr, nullptr);
}
inline void frame(HWND window) {
    BOOL d = dark;
    DwmSetWindowAttribute(window, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &d, sizeof(d));
    COLORREF caption = pal.bg, text = pal.ink;
    DwmSetWindowAttribute(window, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    DwmSetWindowAttribute(window, 36 /* DWMWA_TEXT_COLOR */, &text, sizeof(text));
}
/** Lets popup menus and scroll bars follow the system dark setting (uxtheme ordinals, Windows 10 1903+). */
inline void allow_dark_menus() {
    if (auto ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        using SetMode = int(WINAPI*)(int);
        using Refresh = void(WINAPI*)();
        if (auto f = reinterpret_cast<SetMode>(GetProcAddress(ux, MAKEINTRESOURCEA(135))))
            f(1);
        if (auto f = reinterpret_cast<Refresh>(GetProcAddress(ux, MAKEINTRESOURCEA(104))))
            f();
    }
}
inline void tooltip(HWND owner, HWND tool, const wchar_t* tip) {
    static std::map<HWND, HWND> tips;
    auto& tt = tips[owner];
    if (!tt)
        tt = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0,
                             0, 0, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    TOOLINFOW ti{sizeof(ti)};
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = owner;
    ti.uId = (UINT_PTR)tool;
    ti.lpszText = const_cast<wchar_t*>(tip);
    SendMessageW(tt, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

// ---------------------------------------------------------------- small input dialog
struct PromptData {
    std::wstring title, label, value;
};
inline INT_PTR CALLBACK prompt_proc(HWND d, UINT m, WPARAM w, LPARAM l) {
    auto data = reinterpret_cast<PromptData*>(GetWindowLongPtrW(d, DWLP_USER));
    switch (m) {
    case WM_INITDIALOG: {
        data = reinterpret_cast<PromptData*>(l);
        SetWindowLongPtrW(d, DWLP_USER, l);
        SetWindowTextW(d, data->title.c_str());
        SetDlgItemTextW(d, 101, data->label.c_str());
        SetDlgItemTextW(d, 100, data->value.c_str());
        frame(d);
        for (int id : {IDOK, IDCANCEL, 100})
            SetWindowTheme(GetDlgItem(d, id), dark ? L"DarkMode_Explorer" : nullptr, nullptr);
        auto edit = GetDlgItem(d, 100);
        SetFocus(edit);
        SendMessageW(edit, EM_SETSEL, 0, -1);
        return FALSE;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)w, pal.ink);
        SetBkColor((HDC)w, m == WM_CTLCOLOREDIT ? pal.inset : pal.surface);
        return (INT_PTR)brush(m == WM_CTLCOLOREDIT ? pal.inset : pal.surface);
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            data->value = window_text(GetDlgItem(d, 100));
            EndDialog(d, IDOK);
            return TRUE;
        }
        if (LOWORD(w) == IDCANCEL) {
            EndDialog(d, IDCANCEL);
            return TRUE;
        }
    }
    return FALSE;
}
/** Modal single-line input. Returns nothing when cancelled. */
inline std::optional<std::wstring> prompt(HWND owner, const std::wstring& title, const std::wstring& label,
                                          const std::wstring& initial, const wchar_t* ok = L"确定") {
    std::vector<WORD> t;
    auto dword = [&](DWORD v) { t.push_back(LOWORD(v)); t.push_back(HIWORD(v)); };
    auto str = [&](const wchar_t* s) { while (*s) t.push_back(*s++); t.push_back(0); };
    auto align = [&] { if (t.size() % 2) t.push_back(0); };
    dword(DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU);
    dword(0);
    t.push_back(4);
    for (WORD v : {WORD(0), WORD(0), WORD(300), WORD(92)})
        t.push_back(v);
    t.push_back(0); // menu
    t.push_back(0); // class
    str(L"");
    t.push_back(9);
    str(L"Microsoft YaHei UI");
    auto control = [&](DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls, const wchar_t* text) {
        align();
        dword(style | WS_CHILD | WS_VISIBLE);
        dword(0);
        for (short v : {x, y, cx, cy})
            t.push_back(WORD(v));
        t.push_back(id);
        t.push_back(0xFFFF);
        t.push_back(cls);
        str(text);
        t.push_back(0);
    };
    control(SS_LEFT | SS_NOPREFIX, 12, 10, 276, 20, 101, 0x0082, L"");
    control(ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 12, 34, 276, 15, 100, 0x0081, L"");
    control(BS_DEFPUSHBUTTON | WS_TABSTOP, 160, 64, 62, 18, IDOK, 0x0080, ok);
    control(BS_PUSHBUTTON | WS_TABSTOP, 226, 64, 62, 18, IDCANCEL, 0x0080, L"取消");
    PromptData data{title, label, initial};
    if (DialogBoxIndirectParamW(GetModuleHandleW(nullptr), reinterpret_cast<LPCDLGTEMPLATEW>(t.data()), owner,
                                prompt_proc, reinterpret_cast<LPARAM>(&data)) != IDOK)
        return std::nullopt;
    return data.value;
}

// ---------------------------------------------------------------- lifetime
inline ULONG_PTR gdiplus_token = 0;
inline void startup() {
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&gdiplus_token, &input, nullptr);
    allow_dark_menus();
    load_palette();
}
inline void shutdown() {
    free_fonts();
    free_brushes();
    if (gdiplus_token)
        Gdiplus::GdiplusShutdown(gdiplus_token);
}
/** The app icon (app_icon.rc) scaled down from the nearest larger frame, so it never looks blurry. */
inline HICON app_icon(int metric, UINT at_dpi) {
    int size = GetSystemMetricsForDpi(metric, at_dpi);
    HICON icon = nullptr;
    LoadIconWithScaleDown(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(LANTERN_APP_ICON), size, size, &icon);
    return icon;
}
/** Class icons for registration, at the system DPI. */
inline void class_icons(WNDCLASSEXW& c) {
    UINT system = GetDpiForSystem();
    c.hIcon = app_icon(SM_CXICON, system);
    c.hIconSm = app_icon(SM_CXSMICON, system);
}
/** Caption and taskbar icons at the window's own DPI; call again on WM_DPICHANGED. */
inline void window_icons(HWND h) {
    UINT at = GetDpiForWindow(h);
    for (auto [kind, metric] : {std::pair{ICON_BIG, SM_CXICON}, std::pair{ICON_SMALL, SM_CXSMICON}})
        if (HICON icon = app_icon(metric, at))
            if (auto old = (HICON)SendMessageW(h, WM_SETICON, kind, (LPARAM)icon))
                if (old != (HICON)GetClassLongPtrW(h, kind == ICON_BIG ? GCLP_HICON : GCLP_HICONSM))
                    DestroyIcon(old);
}
/** Clamp the initial window to the monitor work area at the window's own DPI. */
inline void initial_size(HWND h, int w, int hgt) {
    dpi = GetDpiForWindow(h);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
    int mw = mi.rcWork.right - mi.rcWork.left, mh = mi.rcWork.bottom - mi.rcWork.top;
    int cw = std::min(dp(w), mw - dp(40)), ch = std::min(dp(hgt), mh - dp(40));
    SetWindowPos(h, nullptr, mi.rcWork.left + (mw - cw) / 2, mi.rcWork.top + (mh - ch) / 2, cw, ch,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

} // namespace lantern::ui
