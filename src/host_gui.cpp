#include "lantern/host_admin.hpp"
#include "lantern/lan.hpp"
#include "lantern/pairing.hpp"
#include "ui_kit.hpp"
#include <commctrl.h>
#include <commdlg.h>
#include <fstream>
#include <set>
#include <shlobj.h>
#include <thread>
#include <wrl/client.h>
#include <ws2tcpip.h>
using namespace lantern;
namespace {
HWND window, status;
std::map<int, HWND> controls;
/** Page that owns a control: -1 every page, 0 devices, 1 sessions, 2 host settings. */
std::map<int, int> pages;
ui::Scene scene;
std::unique_ptr<HostAdministration> admin;
HostAdminSnapshot snapshot;
fs::path state;
std::unique_ptr<Host> host;
std::thread server, stopper;
bool stopping = false, closing = false;
int page = 2;
std::vector<HostSession> sessions;
std::wstring listen_address;
std::string listen_bind; // what the running host was started with
uint16_t listen_port = 0;
// Private-profile firewall rule for this executable; queried off the UI thread.
FirewallState firewall = FirewallState::unknown;
bool firewall_known = false;
std::thread firewall_worker;
std::wstring network_note;
constexpr UINT firewall_msg = WM_APP + 21;
constexpr wchar_t phone_glyph = 0xE8EA; // Segoe MDL2/Fluent "CellPhone"
constexpr wchar_t globe_glyph = 0xE774; // Segoe MDL2/Fluent "Globe"
uint64_t address_fingerprint = 0;

// Internet access (page 3). `settings` mirrors config.dpapi; the report is gathered off the UI thread.
namespace internet {
InternetSettings settings;
std::optional<ReachabilityReport> report;
bool busy = false;           // a firewall or router request is in flight
bool show_addresses = false; // the readiness card lists every address instead
struct Check {
    ui::Tone tone; // Positive ready, Lamp needs attention, Danger blocks, Muted information only
    std::wstring title, detail;
};
std::vector<Check> checks();
std::wstring headline();
void refresh(); // asks a worker for a fresh report
} // namespace internet

// Presentation strings. Business/status messages stay next to the code that produces them.
namespace ui_text {
const wchar_t* const titles[] = {L"设备与权限", L"当前连接", L"主机设置", L"公网访问"};
const wchar_t* const subtitles[] = {L"逐设备授权；列表显示与主机上限取交集后的生效权限",
                                    L"此窗口启动的主机的实时连接，一台设备可有多个逻辑通道",
                                    L"本机主机身份、授权目录、权限上限与网络监听",
                                    L"让已配对的设备在外面也能连回这台电脑；默认关闭"};
constexpr const wchar_t* identity = L"主机身份与授权目录";
constexpr const wchar_t* ceiling = L"全局权限上限";
constexpr const wchar_t* ceiling_hint = L"任何设备的生效权限都不会超过这里勾选的项目。";
constexpr const wchar_t* network = L"网络与运行";
constexpr const wchar_t* network_hint = L"127.0.0.1 仅本机可连；:: 同时监听 IPv4 和 IPv6（默认）；0.0.0.0 只用 IPv4。";
constexpr const wchar_t* readiness = L"准备情况";
constexpr const wchar_t* addresses = L"本机地址";
constexpr const wchar_t* addresses_hint = L"“使用中”的地址会告诉已配对的设备；临时地址可能每天变化，不会使用。";
constexpr const wchar_t* add_device = L"新增设备";
constexpr const wchar_t* selected_device = L"选中设备";
constexpr const wchar_t* devices_empty = L"还没有设备。在下方输入名称并新增设备。";
constexpr const wchar_t* devices_unloaded = L"尚未加载主机配置。请先在“主机设置”中加载或新建。";
constexpr const wchar_t* sessions_empty = L"暂无连接。";
constexpr const wchar_t* sessions_stopped = L"主机未运行。在“主机设置”中启动后，这里显示实时连接。";
} // namespace ui_text

HWND c(int id) {
    return controls.at(id);
}
bool loopback_bind(const std::string& bind);
uint16_t port_field();
std::wstring text(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n + 1, 0);
    GetWindowTextW(h, s.data(), n + 1);
    s.resize(n);
    return s;
}
void message(const std::wstring& s) {
    SetWindowTextW(status, s.c_str());
}
HWND child(int id, int p, const wchar_t* type, const wchar_t* label, DWORD style = 0) {
    auto h = CreateWindowExW(0, type, label, WS_CHILD | style, 0, 0, 10, 10, window, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(nullptr), nullptr);
    controls[id] = h;
    pages[id] = p;
    SendMessageW(h, WM_SETFONT, (WPARAM)ui::fonts.body, TRUE);
    return h;
}
HWND button(int id, int p, ui::Kind kind, const wchar_t* label, wchar_t glyph = 0,
            ui::Surface surface = ui::Surface::Card) {
    auto h = ui::button(window, id, kind, label, glyph, surface);
    controls[id] = h;
    pages[id] = p;
    return h;
}
void move(int id, int x, int y, int w, int h) {
    MoveWindow(c(id), x, y, w, h, TRUE);
}
void cell(HWND h, int row, int col, const std::wstring& s) {
    if (!col) {
        LVITEMW v{};
        v.mask = LVIF_TEXT;
        v.iItem = row;
        v.pszText = const_cast<wchar_t*>(s.c_str());
        ListView_InsertItem(h, &v);
    } else
        ListView_SetItemText(h, row, col, const_cast<wchar_t*>(s.c_str()));
}
void column(HWND h, int index, const wchar_t* label, int width) {
    LVCOLUMNW v{};
    v.mask = LVCF_TEXT | LVCF_WIDTH;
    v.pszText = const_cast<wchar_t*>(label);
    v.cx = width;
    ListView_InsertColumn(h, index, &v);
}
int selected(HWND h) {
    return ListView_GetNextItem(h, -1, LVNI_SELECTED);
}
std::string device_id() {
    auto i = selected(c(20));
    if (i < 0 || size_t(i) >= snapshot.devices.size())
        throw Error(Code::invalid, "请选择一个设备");
    return snapshot.devices[i].id;
}
uint32_t scopes(int base) {
    uint32_t v = 0;
    for (int i = 0; i < 5; ++i)
        if (SendMessageW(c(base + i), BM_GETCHECK, 0, 0) == BST_CHECKED)
            v |= 1u << i;
    return v;
}
void scopes(int base, uint32_t value) {
    for (int i = 0; i < 5; ++i)
        SendMessageW(c(base + i), BM_SETCHECK, (value & (1u << i)) ? BST_CHECKED : BST_UNCHECKED, 0);
}
std::wstring scope_text(uint32_t v) {
    std::wstring s;
    const wchar_t* labels[] = {L"读", L"写", L"画面", L"控制", L"音频"};
    for (int i = 0; i < 5; ++i)
        if (v & (1u << i)) {
            if (!s.empty())
                s += L" / ";
            s += labels[i];
        }
    return s.empty() ? L"无权限" : s;
}
void hide_secret() {
    SetWindowTextW(c(27), L"");
    SendMessageW(c(28), BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(c(27), EM_SETPASSWORDCHAR, L'●', 0);
    InvalidateRect(c(27), nullptr, TRUE);
}
int rail_width() {
    return ui::dp(212);
}
/** Columns follow the list width; the flex column takes what is left. */
void fit_columns(HWND list, std::initializer_list<int> fixed, int flex_index) {
    RECT r{};
    GetClientRect(list, &r);
    int used = 0, i = 0;
    for (int w : fixed) {
        if (i == flex_index)
            ++i;
        ListView_SetColumnWidth(list, i, ui::dp(w));
        used += ui::dp(w);
        ++i;
    }
    ListView_SetColumnWidth(list, flex_index, std::max(ui::dp(160), int(r.right) - used - ui::dp(4)));
}
void layout() {
    using ui::dp;
    RECT rc{};
    GetClientRect(window, &rc);
    int w = rc.right, h = rc.bottom;
    scene.clear();
    for (auto [id, handle] : controls) {
        int p = pages[id];
        ShowWindow(handle, p == -1 || p == page ? SW_SHOW : SW_HIDE);
    }
    // Rail.
    int rail = rail_width();
    scene.area({0, 0, rail, h}, ui::pal.rail);
    scene.hairlines.push_back({rail - 1, 0, rail, h});
    scene.dots.push_back({dp(30), dp(40), dp(6), ui::pal.lamp});
    scene.text({dp(46), dp(24), rail - dp(12), dp(56)}, L"Lantern 主机", &ui::fonts.heading);
    int navs[] = {52, 50, 51, 53};
    for (int i = 0; i < 4; ++i)
        move(navs[i], dp(12), dp(84) + i * dp(46), rail - dp(24), dp(42));
    // Content frame.
    int x0 = rail + dp(32), R = w - dp(32), y0 = dp(108), bottom = h - dp(60);
    scene.text({x0, dp(22), R, dp(60)}, ui_text::titles[page], &ui::fonts.title);
    scene.text({x0, dp(60), R, dp(84)}, ui_text::subtitles[page], &ui::fonts.body, ui::Tone::Muted);
    scene.hairlines.push_back({rail, h - dp(44), w, h - dp(43)});
    wchar_t info[2] = {ui::glyph::info, 0};
    scene.text({x0, h - dp(43), x0 + dp(20), h}, info, &ui::fonts.icon_small, ui::Tone::Muted,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    move(31, x0 + dp(26), h - dp(43), R - x0 - dp(26), dp(43));

    int bh = dp(36), pad = dp(24);
    if (page == 2) {
        int cw = std::min(R - x0, dp(820)), cx = x0 + pad, cr = x0 + cw - pad;
        int y = y0 + pad;
        scene.text({cx, y, cr, y + dp(24)}, ui_text::identity, &ui::fonts.heading);
        y += dp(36);
        int tools = ui::row_width({c(11), c(12), c(35)}, bh, dp(8));
        bool one = cr - cx - tools - dp(8) >= dp(240);
        int fr = one ? cr - tools - dp(8) : cr;
        ui::field(scene, c(10), {cx, y, fr, y + bh});
        ui::Flow f(one ? fr + dp(8) : cx, cr, one ? y : y + bh + dp(10), dp(8));
        for (int id : {11, 12, 35})
            f.place(c(id), bh);
        y = f.bottom() + dp(12);
        int cw38 = ui::preferred_width(c(38), bh);
        ui::field(scene, c(32), {cx, y, cr - cw38 - dp(10), y + dp(60)});
        move(38, cr - cw38, y + (dp(60) - bh) / 2, cw38, bh);
        y += dp(60) + pad;
        scene.card({x0, y0, x0 + cw, y});

        int top = y + dp(16);
        y = top + pad;
        scene.text({cx, y, cr, y + dp(24)}, ui_text::ceiling, &ui::fonts.heading);
        scene.text({cx, y + dp(26), cr, y + dp(46)}, ui_text::ceiling_hint, &ui::fonts.note, ui::Tone::Muted);
        ui::Flow g(cx, cr, y + dp(56), dp(8));
        for (int id : {100, 101, 102, 103, 104, 17})
            g.place(c(id), dp(34));
        y = g.bottom() + pad;
        scene.card({x0, top, x0 + cw, y});

        top = y + dp(16);
        y = top + pad;
        scene.text({cx, y, cr, y + dp(24)}, ui_text::network, &ui::fonts.heading);
        scene.text({cx, y + dp(26), cr, y + dp(46)}, network_note.empty() ? ui_text::network_hint : network_note,
                   &ui::fonts.note, ui::Tone::Muted);
        ui::Flow n(cx, cr, y + dp(56), dp(10));
        auto labeled = [&](const wchar_t* label, int id, int width) {
            int lw = ui::text_width(ui::fonts.body, label) + dp(10);
            auto r = n.next(lw + width, bh);
            scene.text({r.left, r.top, r.left + lw, r.bottom}, label, &ui::fonts.body, ui::Tone::Muted);
            ui::field(scene, c(id), {r.left + lw, r.top, r.right, r.bottom});
        };
        labeled(L"监听地址", 13, dp(170));
        labeled(L"端口", 14, dp(90));
        labeled(L"共享带宽 Mbps", 36, dp(90));
        n.place(c(37), bh);
        ui::Flow s(cx, cr, n.bottom() + dp(16), dp(10));
        s.place(c(15), dp(40));
        s.place(c(16), dp(40));
        s.place(c(62), dp(40));
        bool firewall_open = firewall_known && firewall == FirewallState::allowed;
        if (firewall_open || loopback_bind(utf8(text(c(13)))))
            ShowWindow(c(63), SW_HIDE);
        else
            s.place(c(63), dp(40));
        y = s.bottom() + pad;
        scene.card({x0, top, x0 + cw, y});
    }
    if (page == 0) {
        int y = y0;
        int dw = ui::preferred_width(c(34), bh);
        scene.card({x0, y, R, y + dp(56)});
        move(19, x0 + dp(18), y + dp(1), R - x0 - dp(48) - dw, dp(54));
        move(34, R - dp(14) - dw, y + dp(10), dw, bh);
        y += dp(56) + dp(12);
        int ix = x0 + pad, ir = R - pad, lw = dp(96);
        // The panel is measured first (apply = false) so the list can take the remaining height.
        auto panel = [&](int top, bool apply) {
            auto put = [&](int id, RECT r) {
                if (apply)
                    move(id, r.left, r.top, r.right - r.left, r.bottom - r.top);
            };
            auto pref = [&](int id, int height) { return ui::preferred_width(c(id), height); };
            int py = top + dp(18);
            if (apply)
                scene.text({ix, py, ix + lw, py + bh}, ui_text::add_device, &ui::fonts.strong);
            ui::Flow a(ix + lw, ir, py, dp(8));
            RECT fr = a.next(dp(240), bh);
            if (apply)
                ui::field(scene, c(21), fr);
            put(22, a.next(pref(22, bh), bh));
            put(60, a.next(pref(60, bh), bh));
            py = a.bottom() + dp(14);
            if (apply)
                scene.hairlines.push_back({ix, py, ir, py + 1});
            py += dp(14);
            if (apply)
                scene.text({ix, py, ix + lw, py + dp(34)}, ui_text::selected_device, &ui::fonts.strong);
            ui::Flow b(ix + lw, ir, py, dp(8));
            for (int id : {200, 201, 202, 203, 204, 23})
                put(id, b.next(pref(id, dp(34)), dp(34)));
            ui::Flow d(ix + lw, ir, b.bottom() + dp(10), dp(8));
            for (int id : {26, 25, 24, 30})
                put(id, d.next(pref(id, bh), bh));
            ui::Flow k(ix + lw, ir, d.bottom() + dp(10), dp(8));
            RECT kr = k.next(std::max(dp(200), std::min(dp(520), ir - ix - lw - pref(28, bh) - dp(8))), bh);
            if (apply)
                ui::field(scene, c(27), kr);
            put(28, k.next(pref(28, bh), bh));
            return k.bottom() + dp(18);
        };
        int note = dp(24);
        int ptop = bottom - note - dp(8) - panel(0, false);
        RECT lc{x0, y, R, std::max(y + dp(120), ptop - dp(12))};
        scene.card(lc);
        move(20, lc.left + dp(6), lc.top + dp(6), lc.right - lc.left - dp(12), lc.bottom - lc.top - dp(12));
        fit_columns(c(20), {200, 260}, 2);
        ptop = std::max(ptop, int(lc.bottom) + dp(12));
        int pb = panel(ptop, true);
        scene.card({x0, ptop, R, pb});
        move(33, x0, pb + dp(6), R - x0, note);
    }
    if (page == 1) {
        int dw = ui::preferred_width(c(41), bh);
        move(43, x0, y0, R - x0, dp(46));
        RECT lc{x0, y0 + dp(54), R, bottom - bh - dp(14)};
        scene.card(lc);
        move(40, lc.left + dp(6), lc.top + dp(6), lc.right - lc.left - dp(12), lc.bottom - lc.top - dp(12));
        fit_columns(c(40), {160, 150, 110, 220}, 4);
        move(41, R - dw, bottom - bh, dw, bh);
    }
    if (page == 3) {
        int cw = std::min(R - x0, dp(820)), cx = x0 + pad, cr = x0 + cw - pad;
        bool on = internet::settings.internet_access;
        auto relabel = [](int id, const wchar_t* label) {
            if (text(c(id)) != label)
                SetWindowTextW(c(id), label);
        };
        relabel(70, on ? L"关闭公网访问" : L"开启公网访问");
        ui::item(c(70))->kind = on ? ui::Kind::Danger : ui::Kind::Primary;
        auto& up = internet::report;
        bool mapped = up && (up->upnp.ipv4.status == UpnpStatus::success || up->upnp.ipv6.status == UpnpStatus::success);
        relabel(75, mapped ? L"撤销路由器放行" : L"让路由器自动放行");
        int y = y0 + pad;
        int tw = ui::preferred_width(c(70), bh);
        scene.text({cx, y, cr - tw - dp(12), y + dp(24)}, on ? L"公网访问已开启" : L"公网访问已关闭",
                   &ui::fonts.heading, on ? ui::Tone::Positive : ui::Tone::Ink);
        move(70, cr - tw, y - dp(6), tw, bh);
        scene.text({cx, y + dp(28), cr - tw - dp(12), y + dp(48)}, internet::headline(), &ui::fonts.note,
                   ui::Tone::Muted);
        ui::Flow n(cx, cr, y + dp(60), dp(10));
        auto labeled = [&](const wchar_t* label, int id, int width) {
            int lw = ui::text_width(ui::fonts.body, label) + dp(10);
            auto r = n.next(lw + width, bh);
            scene.text({r.left, r.top, r.left + lw, r.bottom}, label, &ui::fonts.body, ui::Tone::Muted);
            ui::field(scene, c(id), {r.left + lw, r.top, r.right, r.bottom});
        };
        labeled(L"域名（可选）", 71, dp(250));
        labeled(L"对外端口", 72, dp(110));
        n.place(c(73), bh);
        y = n.bottom() + pad;
        scene.card({x0, y0, x0 + cw, y});

        int top = y + dp(16);
        y = top + pad;
        bool list = internet::show_addresses;
        scene.text({cx, y, cr, y + dp(24)}, list ? ui_text::addresses : ui_text::readiness, &ui::fonts.heading);
        relabel(78, list ? L"返回准备情况" : L"全部地址");
        ui::item(c(78))->glyph = list ? ui::glyph::back : ui::glyph::view;
        int rw = ui::preferred_width(c(76), dp(32)), aw = ui::preferred_width(c(78), dp(32));
        move(76, cr - rw, y - dp(4), rw, dp(32));
        move(78, cr - rw - dp(6) - aw, y - dp(4), aw, dp(32));
        y += dp(34);
        auto rows = internet::checks();
        int body = std::max<int>(dp(28) * int(rows.size()), dp(200));
        ShowWindow(c(77), list ? SW_SHOW : SW_HIDE);
        if (list) {
            scene.text({cx, y, cr, y + dp(20)}, ui_text::addresses_hint, &ui::fonts.note, ui::Tone::Muted);
            move(77, cx - dp(12), y + dp(28), cr - cx + dp(24), body - dp(28));
            fit_columns(c(77), {110, 170, 150}, 0);
            y += body;
        } else {
            int tcol = dp(132);
            for (auto& k : rows) {
                scene.dots.push_back({cx + dp(5), y + dp(13), dp(4), ui::tone(k.tone)});
                scene.text({cx + dp(18), y, cx + tcol, y + dp(26)}, k.title, &ui::fonts.strong);
                scene.text({cx + tcol, y, cr, y + dp(26)}, k.detail, &ui::fonts.body, ui::Tone::Muted);
                y += dp(28);
            }
        }
        auto& r = internet::report;
        bool actions = on && !list;
        bool firewall_button = actions && !(r && r->public_firewall == FirewallState::allowed);
        ShowWindow(c(74), firewall_button ? SW_SHOW : SW_HIDE);
        ShowWindow(c(75), actions ? SW_SHOW : SW_HIDE);
        if (actions) {
            ui::Flow b(cx, cr, y + dp(10), dp(8));
            if (firewall_button)
                b.place(c(74), bh);
            b.place(c(75), bh);
            y = b.bottom();
        }
        y += pad;
        scene.card({x0, top, x0 + cw, y});
    }
    EnableWindow(c(15), admin && !host);
    EnableWindow(c(38), admin && !host);
    EnableWindow(c(60), admin != nullptr);
    EnableWindow(c(62), admin != nullptr);
    EnableWindow(c(16), host && !stopping);
    for (int id : {10, 11, 12, 13, 14, 35})
        EnableWindow(c(id), !host);
    for (int id : {70, 71, 72, 73, 74, 76, 78})
        EnableWindow(c(id), admin && !internet::busy);
    EnableWindow(c(75), host && !stopping && !internet::busy);
    InvalidateRect(window, nullptr, FALSE);
}
void refresh_rail() {
    RECT rc{};
    GetClientRect(window, &rc);
    RECT chip{0, rc.bottom - ui::dp(96), rail_width(), rc.bottom};
    InvalidateRect(window, &chip, FALSE);
    for (int id : {50, 51, 52, 53})
        InvalidateRect(c(id), nullptr, FALSE);
}
/** Host state chip at the foot of the rail. */
void paint_rail(HDC dc) {
    using ui::dp;
    RECT rc{};
    GetClientRect(window, &rc);
    RECT chip{dp(12), rc.bottom - dp(88), rail_width() - dp(12), rc.bottom - dp(16)};
    ui::round_rect(dc, chip, dp(12), ui::pal.surface, ui::pal.line);
    COLORREF dot = host && !stopping ? ui::pal.lamp
                   : stopping        ? ui::mix(ui::pal.lamp, ui::pal.surface, .5)
                                     : ui::pal.faint;
    ui::circle(dc, chip.left + dp(18), chip.top + dp(24), dp(4), dot);
    std::wstring line = host ? (stopping ? L"正在停止…" : L"主机运行中") : admin ? L"已加载，未运行" : L"未加载配置";
    if (host && !stopping && !sessions.empty())
        line += L" · " + std::to_wstring(sessions.size()) + L" 个连接";
    std::wstring detail = host ? listen_address : admin ? state.filename().wstring() : L"在“主机设置”中加载或新建";
    ui::draw_text(dc, line, {chip.left + dp(30), chip.top + dp(12), chip.right - dp(10), chip.top + dp(36)},
                  ui::fonts.strong, ui::pal.ink);
    ui::draw_text(dc, detail, {chip.left + dp(30), chip.top + dp(36), chip.right - dp(10), chip.bottom - dp(10)},
                  ui::fonts.note, ui::pal.muted);
}
void restyle() {
    ui::frame(window);
    for (int id : {20, 40, 77})
        ui::style_list(c(id));
    ui::style_scroll(c(32));
}
void refresh_devices() {
    if (!admin)
        return;
    auto old = selected(c(20));
    std::string id;
    if (old >= 0 && size_t(old) < snapshot.devices.size())
        id = snapshot.devices[old].id;
    hide_secret();
    snapshot = admin->snapshot();
    scopes(100, snapshot.ceiling);
    SetWindowTextW(
        c(32), (L"授权目录：" + snapshot.root.wstring() + L"\r\n证书指纹：" + wide(snapshot.pin)).c_str());
    SetWindowTextW(c(19), snapshot.legacy ? L"旧共享凭据仍启用；可禁用，只保留逐设备信任。"
                                          : L"旧共享凭据已禁用；新连接须使用逐设备密钥。");
    ListView_DeleteAllItems(c(20));
    for (size_t i = 0; i < snapshot.devices.size(); ++i) {
        auto& d = snapshot.devices[i];
        cell(c(20), int(i), 0, wide(d.label));
        cell(c(20), int(i), 1, scope_text(d.effective_scopes));
        cell(c(20), int(i), 2, wide(d.id));
        if (d.id == id)
            ListView_SetItemState(c(20), int(i), LVIS_SELECTED, LVIS_SELECTED);
    }
    layout();
}
void refresh_sessions() {
    std::string selected_id;
    int old = selected(c(40));
    if (old >= 0 && size_t(old) < sessions.size())
        selected_id = sessions[old].id;
    auto next = host ? host->sessions() : std::vector<HostSession>{};
    if (next.size() == sessions.size() &&
        std::equal(next.begin(), next.end(), sessions.begin(), [](auto& a, auto& b) { return a.id == b.id; }))
        return;
    sessions = std::move(next);
    ListView_DeleteAllItems(c(40));
    for (size_t i = 0; i < sessions.size(); ++i) {
        auto& s = sessions[i];
        cell(c(40), int(i), 0, wide(s.label));
        cell(c(40), int(i), 1, wide(s.address));
        const wchar_t* channel[] = {L"控制/文件", L"桌面", L"原始媒体", L"文件传输", L"音频"};
        cell(c(40), int(i), 2, channel[s.channel]);
        cell(c(40), int(i), 3, scope_text(s.scopes));
        cell(c(40), int(i), 4, wide(s.id));
        if (s.id == selected_id)
            ListView_SetItemState(c(40), int(i), LVIS_SELECTED, LVIS_SELECTED);
    }
}
// Remembered between launches: the last loaded state folder and the network fields (never secrets).
// LANTERN_HOST_MANAGER_SETTINGS points elsewhere (tests use their own file); an empty value disables it.
fs::path settings_file() {
    std::wstring buffer(32768, L'\0');
    SetLastError(ERROR_SUCCESS);
    DWORD n = GetEnvironmentVariableW(L"LANTERN_HOST_MANAGER_SETTINGS", buffer.data(), DWORD(buffer.size()));
    if (n || GetLastError() != ERROR_ENVVAR_NOT_FOUND)
        return n && n < buffer.size() ? fs::path(buffer.c_str()) : fs::path();
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
        return {};
    fs::path file = fs::path(local) / L"Lantern" / L"host-manager.ini";
    CoTaskMemFree(local);
    return file;
}
std::map<std::string, std::wstring> read_settings() {
    std::map<std::string, std::wstring> values;
    auto file = settings_file();
    if (file.empty())
        return values;
    std::ifstream in(file, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (auto eq = line.find('='); eq != std::string::npos)
            values[line.substr(0, eq)] = wide(line.substr(eq + 1));
    }
    return values;
}
/** Best effort: a failed write only means the next launch starts from the defaults again. */
void save_settings() {
    auto file = settings_file();
    if (file.empty())
        return;
    auto values = read_settings(); // keeps the remembered folder when nothing is loaded now
    if (admin)
        values["state"] = state.wstring();
    values["bind"] = text(c(13));
    values["port"] = text(c(14));
    values["mbps"] = text(c(36));
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    auto temp = file;
    temp += L".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        for (auto& [key, value] : values)
            out << key << '=' << utf8(value) << '\n';
        if (!out)
            return;
    }
    MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
}
void load() {
    if (host)
        throw Error(Code::busy, "请先停止此窗口启动的主机");
    state = fs::absolute(fs::path(text(c(10))).make_preferred()).lexically_normal();
    auto next = std::make_unique<HostAdministration>(state);
    admin = std::move(next);
    SetWindowTextW(c(10), state.c_str()); // show the normalised path (native separators)
    internet::settings = admin->network_settings();
    internet::report.reset();
    SetWindowTextW(c(71), wide(internet::settings.public_hostname).c_str());
    SetWindowTextW(c(72), internet::settings.external_port ? std::to_wstring(internet::settings.external_port).c_str()
                                                           : L"");
    refresh_devices();
    internet::refresh();
    save_settings();
    message(L"已加载本机主机配置；尚未启动网络监听。");
}
/** Startup: refill the network fields and load the last state folder. Never starts listening. */
void restore_settings() {
    auto values = read_settings();
    for (auto [key, id] : {std::pair{"bind", 13}, std::pair{"port", 14}, std::pair{"mbps", 36}})
        if (auto it = values.find(key); it != values.end() && !it->second.empty())
            SetWindowTextW(c(id), it->second.c_str());
    auto it = values.find("state");
    if (it == values.end() || it->second.empty())
        return;
    fs::path remembered = it->second;
    SetWindowTextW(c(10), remembered.c_str());
    std::error_code ec;
    if (!fs::exists(remembered / L"config.dpapi", ec)) {
        message(L"上次使用的主机状态目录已不存在：" + remembered.wstring() + L"。请重新选择或新建。");
        return;
    }
    try {
        load();
        message(L"已自动加载上次使用的主机配置；尚未启动网络监听。");
    } catch (const std::exception& e) {
        message(L"上次使用的主机配置未能加载：" + wide(e.what()));
    }
}
bool loopback_bind(const std::string& bind) {
    return bind.empty() || bind.starts_with("127.") || bind == "::1" || bind == "localhost";
}
/** The internet settings as the endpoint model wants them; nothing else in Config affects the list. */
Config endpoint_config() {
    Config c;
    c.internet_access = internet::settings.internet_access;
    c.public_hostname = internet::settings.public_hostname;
    c.external_port = internet::settings.external_port;
    return c;
}
/** What a device can try for this listener: the host's own entry list, limited to what `bind` accepts. */
std::vector<ConnectionEndpoint> reachable_endpoints(const Config& config, const std::string& bind,
                                                    uint16_t port) {
    if (loopback_bind(bind))
        return {};
    std::vector<HostAddress> addresses;
    try {
        addresses = host_addresses();
    } catch (...) {
    }
    std::erase_if(addresses, [&](const HostAddress& a) {
        if (bind == "::")
            return false;
        if (bind == "0.0.0.0")
            return a.address.find(':') != std::string::npos;
        return normalize_address(a.address) != normalize_address(bind);
    });
    return host_endpoints(config, port, addresses);
}
/** For people: IPv6 with its zero run compressed (the stored and advertised form keeps every group). */
std::wstring display_host(const std::string& value) {
    IN6_ADDR v6{};
    char out[INET6_ADDRSTRLEN]{};
    if (value.find(':') != std::string::npos && InetPtonA(AF_INET6, value.c_str(), &v6) == 1 &&
        InetNtopA(AF_INET6, &v6, out, sizeof(out)))
        return wide(out);
    return wide(value);
}
std::wstring endpoint_text(const std::string& name, uint16_t port) {
    auto h = display_host(name);
    return (name.find(':') != std::string::npos ? L"[" + h + L"]" : h) + L":" + std::to_wstring(port);
}
std::wstring endpoint_text(const ConnectionEndpoint& e) {
    return endpoint_text(e.host, e.port);
}
/** The bind the network note describes: the running host's real one, otherwise the field. */
std::string current_bind() {
    return host ? listen_bind : utf8(text(c(13)));
}
/** One line under “网络与运行”: who can connect, and whether the firewall lets them. */
void refresh_network_note() {
    auto bind = current_bind();
    if (loopback_bind(bind)) {
        network_note = L"当前只允许本机连接。把监听地址改为 ::，同一局域网里的手机才能连上。";
    } else {
        auto endpoints = reachable_endpoints(endpoint_config(), bind, host ? listen_port : port_field());
        std::erase_if(endpoints, [](auto& e) { return is_public(e.kind); });
        if (endpoints.empty())
            network_note = L"没有找到可用的局域网地址，请确认电脑已连接 Wi-Fi 或网线。";
        else {
            network_note = L"手机可通过 " + endpoint_text(endpoints[0]) + L" 连接";
            if (endpoints.size() > 1)
                network_note += L"（另有 " + std::to_wstring(endpoints.size() - 1) + L" 个地址）";
        }
        if (host && bind == "0.0.0.0" && utf8(text(c(13))) == "::")
            network_note += L" · 未能启用 IPv6，正在只用 IPv4";
        if (internet::settings.internet_access)
            network_note += L" · 公网访问已开启";
        if (!firewall_known)
            network_note += L" · 正在检查防火墙…";
        else if (firewall == FirewallState::allowed)
            network_note += L" · 防火墙已放行（专用网络）";
        else if (firewall == FirewallState::not_allowed)
            network_note += L" · 防火墙可能会拦截手机，请点“允许通过防火墙”";
        else
            network_note += L" · 无法确认防火墙状态";
    }
    if (page == 2)
        layout();
}
fs::path own_executable() {
    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), DWORD(path.size())));
    return path;
}
uint16_t port_field() {
    try {
        auto value = std::stoul(text(c(14)));
        return value && value <= 65535 ? uint16_t(value) : 47443;
    } catch (...) {
        return 47443;
    }
}
/** Queries (and, when asked, requests) the firewall rule off the UI thread. */
void check_firewall(bool request_allow) {
    if (firewall_worker.joinable())
        firewall_worker.join();
    auto exe = own_executable();
    auto port = port_field();
    firewall_known = false;
    refresh_network_note();
    firewall_worker = std::thread([exe, port, request_allow] {
        bool granted = true;
        if (request_allow)
            granted = request_firewall_allow(exe, port);
        auto rule = firewall_state(exe, port);
        PostMessageW(window, firewall_msg, WPARAM(rule), LPARAM(request_allow ? (granted ? 1 : 2) : 0));
    });
}
void stop() {
    if (!host || stopping)
        return;
    stopping = true;
    auto ptr = host.get();
    stopper = std::thread([ptr] { ptr->stop(); });
    message(L"正在终止连接与文件任务…");
    layout();
}
void command(int id);

// ---------------------------------------------------------------- scan-to-pair dialog
// One modeless window. The invitation code exists only in this window's memory and in the QR;
// closing the window cancels the invitation and wipes both.
namespace pairing {
constexpr UINT made_msg = WM_APP + 30;
constexpr int refresh_id = 310, copy_id = 311, done_id = 312, fix_id = 313, remote_id = 314, scope_base = 300;
struct Made {
    std::string id;
    uint64_t expires = 0;
    std::wstring host, uri, error;
    QrMatrix qr;
    std::vector<ConnectionEndpoint> endpoints;
    std::set<std::string> known;
    uint64_t fingerprint = 0;
    bool remote = false;
};
HWND w;
ui::Scene scene;
std::map<int, HWND> controls;
std::thread worker;
std::string invite_id;
uint64_t expires = 0;
std::wstring host_name, uri, status;
ui::Tone tone = ui::Tone::Muted;
QrMatrix qr;
std::vector<ConnectionEndpoint> endpoints;
std::set<std::string> known;
bool busy = false, done = false, remote = false;
uint64_t made_for = 0; // address fingerprint the current code was made for
int ticks = 0;
RECT qr_box{}, info_box{}, status_box{};

enum class Ready { no_config, stopped, loopback, ok };
Ready readiness() {
    if (!admin)
        return Ready::no_config;
    if (!host || stopping)
        return Ready::stopped;
    if (loopback_bind(listen_bind) || reachable_endpoints(endpoint_config(), listen_bind, listen_port).empty())
        return Ready::loopback;
    return Ready::ok;
}
void wipe() {
    std::fill(uri.begin(), uri.end(), L'\0');
    uri.clear();
    qr = {};
    invite_id.clear();
    expires = 0;
}
uint32_t chosen_scopes() {
    uint32_t v = 0;
    for (int i = 0; i < 5; ++i)
        if (SendMessageW(controls[scope_base + i], BM_GETCHECK, 0, 0) == BST_CHECKED)
            v |= 1u << i;
    return v;
}
void join() {
    if (worker.joinable())
        worker.join();
}
void layout();
/** Cancels the previous invitation and creates a new one, all on a worker thread. */
void generate() {
    if (busy || readiness() != Ready::ok)
        return;
    auto scopes = chosen_scopes();
    if (!scopes) {
        wipe();
        status = L"请至少选择一项权限。";
        tone = ui::Tone::Danger;
        layout();
        return;
    }
    join();
    busy = true;
    done = false;
    status = L"正在生成二维码…";
    tone = ui::Tone::Muted;
    auto old = invite_id;
    wipe();
    layout();
    // Remote redemption only while Internet access is on, and only when asked for this code.
    bool allow_remote = remote && internet::settings.internet_access;
    worker = std::thread([old, st = state, config = endpoint_config(), bind = listen_bind, port = listen_port,
                          scopes, allow_remote] {
        auto made = std::make_unique<Made>();
        try {
            HostAdministration a(st);
            if (!old.empty())
                try {
                    a.cancel_invite(old);
                } catch (...) {
                }
            for (auto& d : a.snapshot().devices)
                made->known.insert(d.id);
            made->fingerprint = host_address_fingerprint();
            made->endpoints = reachable_endpoints(config, bind, port);
            if (made->endpoints.empty())
                throw Error(Code::io, "没有找到可用的局域网地址");
            auto invite = a.invite("扫码配对", scopes, std::chrono::minutes(10), allow_remote);
            made->id = invite.id;
            made->expires = invite.expires_at;
            made->host = wide(invite.host_name);
            made->remote = allow_remote;
            // A local-only code is redeemed on the LAN, and the device asks for public entries once it is
            // connected. Plain IPv4 keeps the code small and the v1 format that v0.2 phones can read.
            std::vector<std::string> v4;
            for (auto& e : made->endpoints)
                if (e.kind == EndpointKind::lan_v4 && e.port == port)
                    v4.push_back(e.host);
            if (!allow_remote)
                std::erase_if(made->endpoints, [](auto& e) { return is_public(e.kind); });
            if (!allow_remote && !v4.empty())
                std::erase_if(made->endpoints, [](auto& e) { return e.kind != EndpointKind::lan_v4; });
            auto link = !allow_remote && !v4.empty() ? pairing_uri(invite, v4, port)
                                                     : pairing_uri_endpoints(invite, made->endpoints, port);
            made->qr = qr_encode(link);
            made->uri = wide(link);
            std::fill(link.begin(), link.end(), '\0');
            std::fill(invite.code.begin(), invite.code.end(), '\0');
        } catch (const std::exception& e) {
            made->error = wide(e.what());
        }
        if (PostMessageW(w, made_msg, 0, LPARAM(made.get())))
            made.release();
    });
}
void close() {
    if (!w)
        return;
    join();
    if (!invite_id.empty() && !done)
        try {
            HostAdministration(state).cancel_invite(invite_id);
        } catch (...) {
        }
    wipe();
    auto h = w;
    w = nullptr;
    DestroyWindow(h);
}
/** Called every two seconds: has the phone used the invitation yet? */
void watch() {
    if (invite_id.empty() || done || busy)
        return;
    try {
        HostAdministration a(state);
        auto pending = a.invites();
        if (std::any_of(pending.begin(), pending.end(), [](auto& p) { return p.id == invite_id; }))
            return;
        if (pairing_unix_time() >= expires)
            return; // expired, not used; the countdown says so
        std::wstring label = L"新设备";
        for (auto& d : a.snapshot().devices)
            if (!known.count(d.id))
                label = wide(d.label);
        done = true;
        status = L"已添加“" + label + L"”。手机上会自动连接，以后打开就能直接用。";
        tone = ui::Tone::Positive;
        wipe();
        refresh_devices();
        layout();
    } catch (...) {
    }
}
void copy_link() {
    if (uri.empty() || !OpenClipboard(w))
        return;
    EmptyClipboard();
    size_t bytes = (uri.size() + 1) * sizeof(wchar_t);
    if (auto mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(mem), uri.c_str(), bytes);
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem))
            GlobalFree(mem);
    }
    CloseClipboard();
    status = L"链接已复制。可粘贴到 Windows 客户端，和二维码一样只能用一次。";
    tone = ui::Tone::Muted;
    InvalidateRect(w, &status_box, FALSE);
}
void layout() {
    if (!w)
        return;
    using ui::dp;
    RECT rc{};
    GetClientRect(w, &rc);
    int W = rc.right, H = rc.bottom, x = dp(28), R = W - dp(28);
    scene.clear();
    int y = dp(20);
    scene.text({x, y, R, y + dp(32)}, L"用手机扫码添加设备", &ui::fonts.title);
    y += dp(36);
    scene.text({x, y, R, y + dp(20)}, L"在手机 Lantern 的“设备”页点“扫码配对”，对准下面的二维码。", &ui::fonts.note,
               ui::Tone::Muted);
    y += dp(32);
    int q = dp(272);
    qr_box = {(W - q) / 2, y, (W + q) / 2, y + q};
    y += q + dp(10);
    info_box = {x, y, R, y + dp(44)};
    y += dp(54);
    scene.text({x, y, R, y + dp(22)}, L"这台设备可以", &ui::fonts.strong);
    ui::Flow f(x, R, y + dp(28), dp(8));
    for (int i = 0; i < 5; ++i)
        f.place(controls[scope_base + i], dp(34));
    y = f.bottom() + dp(10);
    bool internet_on = internet::settings.internet_access;
    ShowWindow(controls[remote_id], internet_on ? SW_SHOW : SW_HIDE);
    if (internet_on) {
        ui::Flow g(x, R, y, dp(8));
        g.place(controls[remote_id], dp(34));
        y = g.bottom() + dp(10);
    }
    status_box = {x, y, R, y + dp(40)};
    int bh = dp(36), by = H - dp(24) - bh;
    ui::Flow b(x, R, by, dp(8));
    b.place(controls[refresh_id], bh);
    b.place(controls[copy_id], bh);
    int dw = ui::preferred_width(controls[done_id], bh);
    MoveWindow(controls[done_id], R - dw, by, dw, bh, TRUE);
    auto ready = readiness();
    ShowWindow(controls[fix_id], ready == Ready::stopped || ready == Ready::loopback ? SW_SHOW : SW_HIDE);
    int fw = ui::preferred_width(controls[fix_id], bh);
    MoveWindow(controls[fix_id], (W - fw) / 2, qr_box.bottom - dp(60), fw, bh, TRUE);
    EnableWindow(controls[refresh_id], ready == Ready::ok && !busy);
    EnableWindow(controls[copy_id], !uri.empty());
    InvalidateRect(w, nullptr, FALSE);
}
void paint(HDC dc) {
    using ui::dp;
    auto ready = readiness();
    bool live = !qr.modules.empty() && !done;
    uint64_t now = pairing_unix_time();
    bool expired = live && now >= expires;
    ui::round_rect(dc, qr_box, dp(16), live ? RGB(255, 255, 255) : ui::pal.inset, ui::pal.line);
    if (live) {
        // Quiet zone of four modules, integer module size, no smoothing.
        int total = qr.size + 8, inner = qr_box.right - qr_box.left - dp(16);
        int m = std::max(1, inner / total), side = m * total;
        int ox = qr_box.left + (qr_box.right - qr_box.left - side) / 2 + 4 * m;
        int oy = qr_box.top + (qr_box.bottom - qr_box.top - side) / 2 + 4 * m;
        HBRUSH ink = ui::brush(expired ? RGB(200, 204, 210) : RGB(0, 0, 0));
        for (int r = 0; r < qr.size; ++r)
            for (int c0 = 0; c0 < qr.size;) {
                if (!qr.modules[size_t(r) * qr.size + c0]) {
                    ++c0;
                    continue;
                }
                int c1 = c0;
                while (c1 < qr.size && qr.modules[size_t(r) * qr.size + c1])
                    ++c1;
                RECT cell{ox + c0 * m, oy + r * m, ox + c1 * m, oy + (r + 1) * m};
                FillRect(dc, &cell, ink);
                c0 = c1;
            }
        if (expired) {
            RECT badge{qr_box.left + dp(70), qr_box.top + dp(116), qr_box.right - dp(70), qr_box.top + dp(156)};
            ui::round_rect(dc, badge, dp(10), ui::pal.surface, ui::pal.line);
            ui::draw_text(dc, L"已过期", badge, ui::fonts.strong, ui::pal.ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    } else {
        std::wstring big, sub;
        if (done) {
            big = L"配对完成";
            sub = L"可以关闭这个窗口了";
        } else if (ready == Ready::no_config) {
            big = L"还没有主机配置";
            sub = L"请先在“主机设置”里加载或新建";
        } else if (ready == Ready::stopped) {
            big = L"主机还没有启动";
            sub = L"手机需要连到正在运行的主机";
        } else if (ready == Ready::loopback) {
            big = L"主机只允许本机连接";
            sub = L"改为局域网监听后，手机才能连上";
        } else {
            big = busy ? L"正在生成…" : L"";
        }
        RECT t{qr_box.left + dp(16), qr_box.top + dp(70), qr_box.right - dp(16), qr_box.top + dp(104)};
        ui::draw_text(dc, big, t, ui::fonts.heading, done ? ui::pal.positive : ui::pal.ink,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        OffsetRect(&t, 0, dp(34));
        ui::draw_text(dc, sub, t, ui::fonts.note, ui::pal.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (live) {
        std::wstring line1;
        if (expired)
            line1 = L"二维码已过期，点“刷新二维码”生成新的";
        else {
            auto left = expires - now;
            wchar_t clock[16];
            swprintf_s(clock, L"%02llu:%02llu", left / 60, left % 60);
            line1 = std::wstring(clock) + L" 后失效 · 只能使用一次";
        }
        std::wstring line2 = host_name;
        if (!endpoints.empty())
            line2 += L" · " + endpoint_text(endpoints[0]);
        if (endpoints.size() > 1)
            line2 += L" 等 " + std::to_wstring(endpoints.size()) + L" 个地址";
        if (internet::settings.internet_access)
            line2 += remote ? L" · 外网也可扫码" : L" · 仅限局域网扫码";
        RECT a = info_box;
        a.bottom = a.top + ui::dp(22);
        ui::draw_text(dc, line1, a, ui::fonts.body, expired ? ui::pal.danger : ui::pal.ink,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        OffsetRect(&a, 0, ui::dp(22));
        ui::draw_text(dc, line2, a, ui::fonts.note, ui::pal.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    ui::draw_text(dc, status, status_box, ui::fonts.body, ui::tone(tone), DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
}
LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    try {
        switch (msg) {
        case WM_CREATE: {
            w = h;
            const wchar_t* labels[] = {L"文件读取", L"文件写入", L"观看桌面", L"键鼠控制", L"系统音频"};
            // Least privilege by default: everything the host allows except writing files.
            uint32_t initial = snapshot.ceiling & ~uint32_t(files_write);
            if (!initial)
                initial = snapshot.ceiling;
            for (int i = 0; i < 5; ++i) {
                controls[scope_base + i] = ui::button(h, scope_base + i, ui::Kind::Chip, labels[i]);
                SendMessageW(controls[scope_base + i], BM_SETCHECK, (initial >> i) & 1 ? BST_CHECKED : BST_UNCHECKED, 0);
                EnableWindow(controls[scope_base + i], (snapshot.ceiling >> i) & 1);
            }
            controls[refresh_id] = ui::button(h, refresh_id, ui::Kind::Secondary, L"刷新二维码", ui::glyph::refresh);
            controls[copy_id] = ui::button(h, copy_id, ui::Kind::Ghost, L"复制链接", ui::glyph::copy);
            controls[done_id] = ui::button(h, done_id, ui::Kind::Primary, L"完成");
            controls[fix_id] = ui::button(h, fix_id, ui::Kind::Primary, L"改为局域网并启动主机", ui::glyph::play);
            controls[remote_id] = ui::button(h, remote_id, ui::Kind::Chip, L"允许在外网扫码");
            for (auto [id, c] : controls)
                ShowWindow(c, SW_SHOW);
            ui::frame(h);
            SetTimer(h, 1, 1000, nullptr);
            layout();
            generate();
            return 0;
        }
        case WM_TIMER:
            if (++ticks % 2 == 0)
                watch();
            if (!busy && !done && invite_id.empty() && readiness() == Ready::ok && status.empty())
                generate(); // the host just became reachable
            InvalidateRect(h, &info_box, FALSE);
            if (!qr.modules.empty() && pairing_unix_time() >= expires)
                InvalidateRect(h, &qr_box, FALSE);
            return 0;
        case made_msg: {
            std::unique_ptr<Made> made(reinterpret_cast<Made*>(lp));
            busy = false;
            if (!made->error.empty()) {
                status = L"未能生成二维码：" + made->error;
                tone = ui::Tone::Danger;
            } else {
                invite_id = made->id;
                expires = made->expires;
                host_name = made->host;
                uri = std::move(made->uri);
                qr = std::move(made->qr);
                endpoints = made->endpoints;
                known = made->known;
                made_for = made->fingerprint;
                remote = made->remote;
                SendMessageW(controls[remote_id], BM_SETCHECK, remote ? BST_CHECKED : BST_UNCHECKED, 0);
                status = L"等待手机扫码…";
                tone = ui::Tone::Muted;
            }
            layout();
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (HIWORD(wp) != BN_CLICKED)
                return 0;
            if (lp)
                ui::clicked(HWND(lp));
            if (id >= scope_base && id < scope_base + 5) {
                generate();
            } else if (id == remote_id) {
                bool want = SendMessageW(controls[remote_id], BM_GETCHECK, 0, 0) == BST_CHECKED;
                if (want && MessageBoxW(h,
                                        L"允许这个二维码在外网使用？\n\n"
                                        L"10 分钟内，拿到二维码或链接的人（包括看到截图的人）可以在任何地方把自己的设备加进来。\n\n"
                                        L"只有手机不在家、又必须现在配对时才需要打开。用完请关闭这个窗口。",
                                        L"外网扫码", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
                    SendMessageW(controls[remote_id], BM_SETCHECK, BST_UNCHECKED, 0);
                    return 0;
                }
                remote = want;
                generate();
            } else if (id == refresh_id) {
                generate();
            } else if (id == copy_id) {
                copy_link();
            } else if (id == fix_id) {
                if (loopback_bind(utf8(text(c(13)))))
                    SetWindowTextW(c(13), wide(default_host_bind).c_str());
                if (host) {
                    status = L"请先在“主机设置”里停止主机，再改为局域网监听。";
                    tone = ui::Tone::Danger;
                    layout();
                } else {
                    status.clear();
                    SendMessageW(window, WM_COMMAND, 15, 0);
                    layout();
                }
            } else if (id == done_id || id == IDCANCEL) {
                close();
            }
            return 0;
        }
        case WM_DRAWITEM:
            ui::draw((DRAWITEMSTRUCT*)lp);
            return TRUE;
        case WM_PAINT:
            ui::paint(h, scene, paint);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_CLOSE:
            close();
            return 0;
        case WM_DESTROY:
            KillTimer(h, 1);
            controls.clear();
            return 0;
        }
    } catch (const std::exception& e) {
        status = wide(e.what());
        tone = ui::Tone::Danger;
        InvalidateRect(h, nullptr, FALSE);
    }
    return DefWindowProcW(h, msg, wp, lp);
}
void open() {
    if (w) {
        SetForegroundWindow(w);
        return;
    }
    if (!admin)
        throw Error(Code::invalid, "请先在“主机设置”中加载或新建主机配置");
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"LanternPairing";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        ui::class_icons(wc);
        RegisterClassExW(&wc);
        registered = true;
    }
    status.clear();
    done = false;
    busy = false;
    remote = false;
    ticks = 0;
    RECT client{0, 0, ui::dp(468), ui::dp(internet::settings.internet_access ? 704 : 660)};
    AdjustWindowRectExForDpi(&client, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME, ui::dpi);
    RECT owner{};
    GetWindowRect(window, &owner);
    int ww = client.right - client.left, wh = client.bottom - client.top;
    int x = owner.left + (owner.right - owner.left - ww) / 2, y = owner.top + (owner.bottom - owner.top - wh) / 2;
    CreateWindowExW(WS_EX_DLGMODALFRAME, L"LanternPairing", L"扫码添加设备", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                    x, std::max(0, y), ww, wh, window, nullptr, GetModuleHandleW(nullptr), nullptr);
    ui::window_icons(w);
    ShowWindow(w, SW_SHOW);
}
/** The main window started or stopped the host: re-evaluate what the dialog can show. */
void host_changed() {
    if (!w)
        return;
    if (readiness() != Ready::ok && !done) {
        if (!invite_id.empty())
            try {
                HostAdministration(state).cancel_invite(invite_id);
            } catch (...) {
            }
        wipe();
        status.clear();
    }
    layout();
}
/** The PC's addresses changed (new IPv6 prefix, another Wi-Fi): a live code would point at stale ones. */
void addresses_changed(uint64_t now) {
    if (w && !done && !busy && !invite_id.empty() && now != made_for)
        generate();
}
} // namespace pairing

// ---------------------------------------------------------------- Internet access page
namespace internet {
constexpr UINT report_msg = WM_APP + 40, action_msg = WM_APP + 41;
enum class Action { firewall_add, firewall_remove, upnp_map, upnp_unmap };
struct Done {
    Action action;
    bool ok = false;
    UpnpReport upnp;
    std::wstring error;
};
// `probe` touches the running Host (report, router); joined before the Host is destroyed.
// `elevate` only waits for the UAC-elevated CLI and never touches the Host.
std::thread probe, elevate;
bool checking = false, again = false;
bool router_wanted = false; // the user asked the router this run; re-asked when addresses change
NetworkSecurityStats stats;
const wchar_t* upnp_text(UpnpStatus s, bool v6) {
    switch (s) {
    case UpnpStatus::success: return v6 ? L"路由器已放行 IPv6" : L"路由器已映射 IPv4 端口";
    case UpnpStatus::no_igd: return L"没有发现支持 UPnP 的路由器";
    case UpnpStatus::unsupported: return v6 ? L"路由器不支持自动放行 IPv6" : L"路由器不支持端口映射";
    case UpnpStatus::denied: return L"路由器拒绝了自动配置";
    case UpnpStatus::cgnat: return L"没有公网 IPv4（运营商 NAT），已跳过";
    case UpnpStatus::network_error: return L"与路由器通信失败";
    case UpnpStatus::expired: return L"路由器放行已过期";
    default: return L"未请求";
    }
}
bool router_active() {
    return report && (report->upnp.ipv4.status == UpnpStatus::success ||
                      report->upnp.ipv6.status == UpnpStatus::success);
}
void join_probe() {
    if (probe.joinable())
        probe.join();
}
void refresh() {
    if (!admin)
        return;
    if (checking) {
        again = true;
        return;
    }
    join_probe();
    checking = true;
    auto exe = own_executable();
    auto running = host && !stopping ? host.get() : nullptr;
    probe = std::thread([running, exe, st = state, bind = utf8(text(c(13))), port = port_field()] {
        auto made = std::make_unique<ReachabilityReport>();
        try {
            *made = running ? running->reachability_report(exe)
                            : reachability_report(load_config(st / L"config.dpapi"), exe, bind, port, false);
        } catch (...) {
            made.reset();
        }
        if (PostMessageW(window, report_msg, 0, LPARAM(made.get())))
            made.release();
    });
}
void finish(Done& d);
/** The UAC prompt can stay open for a minute, so this runs on its own thread. */
void request_firewall(bool allow) {
    if (elevate.joinable())
        elevate.join();
    busy = true;
    message(allow ? L"正在请求管理员权限，添加“Lantern 主机（公网）”防火墙规则…"
                  : L"正在请求管理员权限，删除“Lantern 主机（公网）”防火墙规则…");
    layout();
    elevate = std::thread([allow, exe = own_executable(), port = host ? listen_port : port_field()] {
        auto d = std::make_unique<Done>();
        d->action = allow ? Action::firewall_add : Action::firewall_remove;
        d->ok = request_public_firewall(exe, port, allow);
        if (PostMessageW(window, action_msg, 0, LPARAM(d.get())))
            d.release();
    });
}
void router(bool map) {
    if (!host || stopping)
        throw Error(Code::invalid, "请先启动主机");
    join_probe();
    busy = true;
    message(map ? L"正在询问路由器…" : L"正在撤销路由器放行…");
    layout();
    probe = std::thread([map, running = host.get()] {
        auto d = std::make_unique<Done>();
        d->action = map ? Action::upnp_map : Action::upnp_unmap;
        try {
            d->upnp = map ? running->upnp_map() : running->upnp_unmap();
            d->ok = true;
        } catch (const std::exception& e) {
            d->error = wide(e.what());
        }
        if (PostMessageW(window, action_msg, 0, LPARAM(d.get())))
            d.release();
    });
}
/** Hostname and public port from the page; empty port means “same as the listening port”. */
std::pair<std::string, uint16_t> fields() {
    auto name = utf8(text(c(71)));
    name.erase(0, name.find_first_not_of(' '));
    name.erase(name.find_last_not_of(' ') + 1);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char ch) { return char(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch); });
    if (!name.empty() && !valid_hostname(name))
        throw Error(Code::invalid, "域名格式无效：只填主机名，例如 home.example.com，不要带 http:// 或端口");
    auto value = text(c(72));
    uint16_t port = 0;
    if (!value.empty()) {
        auto n = std::stoul(value);
        if (!n || n > 65535)
            throw Error(Code::invalid, "对外端口必须为 1..65535，或留空表示和监听端口相同");
        port = uint16_t(n);
    }
    return {name, port};
}
void reload() {
    settings = admin->network_settings();
    refresh_network_note();
    pairing::host_changed();
    refresh();
    layout();
}
void save_fields() {
    auto [name, port] = fields();
    admin->internet(settings.internet_access, name, port);
    reload();
    message(name.empty() ? L"已保存。没有填写域名时，手机只用电脑当前的公网 IPv6 地址。"
                         : L"已保存。域名的 DNS 记录需要你自己的动态域名服务来更新，Lantern 只负责把它告诉手机。");
}
void enable() {
    if (!admin)
        throw Error(Code::invalid, "请先在“主机设置”中加载或新建主机配置");
    auto [name, port] = fields();
    if (snapshot.legacy) {
        if (MessageBoxW(window,
                        L"开启公网访问前，需要先禁用旧共享凭据。\n\n仍在使用旧密钥的连接会断开；"
                        L"逐设备配对的手机和电脑不受影响。\n\n现在禁用并继续？",
                        L"禁用旧共享凭据", MB_YESNO | MB_ICONWARNING) != IDYES)
            return;
        admin->disable_legacy();
        refresh_devices();
    }
    if (MessageBoxW(window,
                    L"开启后，这台电脑会接受来自互联网的连接：\n\n"
                    L"· 只有已配对的设备能登录，每台设备的权限不变；\n"
                    L"· 握手超时、来源限流、失败退避等公网防护会同时启用；\n"
                    L"· 还需要 Windows 防火墙和路由器放行，外网才连得进来。\n\n"
                    L"新设备仍建议在家里扫码配对。继续？",
                    L"开启公网访问", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    admin->internet(true, name, port);
    // An explicit IPv4 listener cannot be reached over IPv6; offer dual stack, never change it silently.
    if (!host && utf8(text(c(13))) == "0.0.0.0" &&
        MessageBoxW(window, L"当前监听地址是 0.0.0.0，只接受 IPv4。把它改为 ::，同时接受 IPv4 和 IPv6？",
                    L"监听地址", MB_YESNO | MB_ICONQUESTION) == IDYES) {
        SetWindowTextW(c(13), wide(default_host_bind).c_str());
        save_settings();
    }
    reload();
    message(host && listen_bind != "::"
                ? L"公网访问已开启。主机正在只用 IPv4 监听；停止主机、把监听地址改为 :: 后再启动，才能用 IPv6。"
                : L"公网访问已开启。下一步：添加公网防火墙规则，并确认路由器放行。");
}
void finish_disable() {
    admin->internet(false, settings.public_hostname, settings.external_port);
    bool unmap = host && !stopping && router_active();
    router_wanted = false;
    reload();
    if (unmap)
        router(false);
    message(L"公网访问已关闭，只接受局域网连接。");
}
void disable() {
    // The rule is removed first; if that is refused, the hardened mode stays on rather than pretending.
    if (!report || report->public_firewall != FirewallState::not_allowed)
        request_firewall(false);
    else
        finish_disable();
}
void finish(Done& d) {
    busy = false;
    switch (d.action) {
    case Action::firewall_add:
        message(d.ok ? L"已添加公网防火墙规则（仅专用网络，TCP）。"
                     : L"没有添加防火墙规则（未获得管理员授权或操作失败）。");
        break;
    case Action::firewall_remove:
        if (d.ok) {
            finish_disable();
            return;
        }
        MessageBoxW(window,
                    L"没能删除公网防火墙规则（未获得管理员授权或操作失败），所以公网访问仍保持开启，防护继续有效。\n\n"
                    L"你可以：\n· 再点一次“关闭公网访问”重试；\n· 或停止主机；\n"
                    L"· 或在“Windows Defender 防火墙 → 高级设置 → 入站规则”里删除“Lantern 主机（公网）”，再回来关闭。",
                    L"公网访问仍开启", MB_OK | MB_ICONWARNING);
        message(L"公网防火墙规则没有删除，公网访问保持开启。");
        break;
    case Action::upnp_map:
    case Action::upnp_unmap: {
        if (!d.ok) {
            message(L"路由器：" + d.error);
            break;
        }
        router_wanted = d.action == Action::upnp_map;
        auto port = std::to_wstring(d.upnp.ipv6.external_port ? d.upnp.ipv6.external_port : listen_port);
        if (d.action == Action::upnp_unmap)
            message(L"已撤销路由器放行。");
        else if (d.upnp.ipv4.status == UpnpStatus::success || d.upnp.ipv6.status == UpnpStatus::success)
            message(std::wstring(L"IPv6：") + upnp_text(d.upnp.ipv6.status, true) + L"；IPv4：" +
                    upnp_text(d.upnp.ipv4.status, false) + L"。主机运行期间会自动续期，停止时撤销。");
        else
            message(std::wstring(L"路由器没有自动放行（") + upnp_text(d.upnp.ipv6.status, true) +
                    L"）。请在路由器的 IPv6 防火墙里手动放行 TCP 端口 " + port + L"。");
        break;
    }
    }
    refresh();
    layout();
}
void toggle() {
    if (settings.internet_access)
        disable();
    else
        enable();
}
/** New addresses (a new IPv6 prefix): re-check, and renew the router's permission if it was asked. */
void addresses_changed() {
    refresh();
    if (router_wanted && host && !stopping && !busy && settings.internet_access)
        try {
            router(true);
        } catch (...) {
        }
}
std::wstring minutes_left(uint64_t expires) {
    auto now = pairing_unix_time();
    return expires > now ? L"，剩余 " + std::to_wstring((expires - now + 59) / 60) + L" 分钟" : L"";
}
std::wstring headline() {
    if (!admin)
        return L"请先在“主机设置”中加载或新建主机配置。";
    if (!settings.internet_access)
        return L"关闭时只接受局域网连接，和 v0.2 完全一样。开启前会先禁用旧共享凭据。";
    int blocking = 0;
    for (auto& k : checks())
        blocking += k.tone == ui::Tone::Danger || k.tone == ui::Tone::Lamp;
    if (!host)
        return L"已开启。启动主机后，已配对的设备在外面也能连回来。";
    if (!blocking)
        return L"本机侧已准备好。请用手机的移动数据实际连一次确认。";
    return L"还有 " + std::to_wstring(blocking) + L" 项需要处理，见下方。";
}
std::vector<Check> checks() {
    using ui::Tone;
    std::vector<Check> out;
    if (!admin)
        return out;
    if (!report) {
        out.push_back({Tone::Muted, L"检查中", L"正在读取本机网络状态…"});
        return out;
    }
    auto& r = *report;
    auto port = std::to_wstring(r.port);
    // Listener.
    if (!r.listening)
        out.push_back({Tone::Muted, L"主机监听", r.bind == "::" ? L"主机未运行；启动后同时监听 IPv4 和 IPv6"
                                                              : L"主机未运行"});
    else if (r.dual_stack)
        out.push_back({Tone::Positive, L"主机监听", L"IPv4 + IPv6 双栈，端口 " + port});
    else if (r.bind == "0.0.0.0")
        out.push_back({Tone::Lamp, L"主机监听",
                       utf8(text(c(13))) == "::" ? L"未能启用 IPv6，正在只用 IPv4"
                                                 : L"只监听 IPv4；停止主机，把监听地址改为 :: 后再启动"});
    else
        out.push_back({Tone::Lamp, L"主机监听", L"只监听 " + wide(r.bind) + L"；改为 :: 才能从公网连接"});
    // IPv6.
    std::vector<const HostAddress*> stable6;
    bool temporary6 = false, public4 = false;
    for (auto& a : r.addresses) {
        if (a.category == AddressKind::global_v6 && !a.deprecated) {
            if (a.temporary)
                temporary6 = true;
            else
                stable6.push_back(&a);
        }
        if (a.category == AddressKind::public_v4 && !a.deprecated)
            public4 = true;
    }
    if (!stable6.empty())
        out.push_back({Tone::Positive, L"公网 IPv6",
                       display_host(stable6.front()->address) +
                           (stable6.size() > 1 ? L" 等 " + std::to_wstring(stable6.size()) + L" 个" : L"")});
    else if (temporary6)
        out.push_back({Tone::Lamp, L"公网 IPv6", L"只有临时地址，可能每天变化；请检查路由器的 IPv6 设置"});
    else
        out.push_back({Tone::Lamp, L"公网 IPv6", L"暂未发现公网 IPv6；等待网络，或检查路由器是否开启 IPv6"});
    // IPv4.
    auto& v4 = r.upnp.ipv4;
    if (v4.status == UpnpStatus::success)
        out.push_back({Tone::Positive, L"公网 IPv4",
                       endpoint_text(v4.external_address, v4.external_port) + L"（路由器映射" +
                           minutes_left(v4.expires_at) + L"）"});
    else if (public4)
        out.push_back({Tone::Positive, L"公网 IPv4", L"这台电脑直接拥有公网 IPv4 地址"});
    else if (r.cgnat)
        out.push_back({Tone::Muted, L"公网 IPv4", L"没有公网 IPv4（运营商 NAT），可以只用 IPv6"});
    else
        out.push_back({Tone::Muted, L"公网 IPv4", L"未检测到；有公网 IPv4 时可让路由器做端口映射"});
    // Legacy credential.
    if (r.legacy_enabled)
        out.push_back({settings.internet_access ? Tone::Danger : Tone::Muted, L"旧共享凭据",
                       L"仍启用；开启公网访问时会先禁用"});
    else
        out.push_back({Tone::Positive, L"旧共享凭据", L"已禁用，只接受逐设备凭据"});
    // Firewall.
    // NET_FW_PROFILE2_PRIVATE = 2, PUBLIC = 4. Several adapters can be active; warn when none is Private.
    bool public_only = (r.current_profiles & 4) && !(r.current_profiles & 2);
    auto attention = settings.internet_access ? Tone::Lamp : Tone::Muted; // only matters once it is on
    if (public_only)
        out.push_back({attention, L"防火墙",
                       L"网络被识别为“公用”，Lantern 的规则不在此生效；如果是自家网络，可在 Windows 设置里改为专用网络"});
    else if (r.public_firewall == FirewallState::allowed)
        out.push_back({Tone::Positive, L"防火墙", L"已放行 TCP " + port + L"（仅专用网络）"});
    else if (r.public_firewall == FirewallState::not_allowed)
        out.push_back({settings.internet_access ? Tone::Danger : Tone::Muted, L"防火墙",
                       L"没有公网规则，外网连接会被拦截"});
    else
        out.push_back({attention, L"防火墙", L"无法确认（可能由组策略管理）"});
    // Router.
    auto& v6 = r.upnp.ipv6;
    if (v6.status == UpnpStatus::success)
        out.push_back({Tone::Positive, L"路由器", std::wstring(L"已自动放行 IPv6") + minutes_left(v6.expires_at)});
    else if (v6.status == UpnpStatus::idle)
        out.push_back({Tone::Muted, L"路由器",
                       L"路由器通常会拦截外网访问：点“让路由器自动放行”，或在路由器里手动放行 TCP " + port});
    else
        out.push_back({attention, L"路由器",
                       std::wstring(upnp_text(v6.status, true)) + L"；请在路由器里手动放行 TCP " + port});
    // Hostname.
    if (settings.public_hostname.empty())
        out.push_back({Tone::Muted, L"域名", L"未设置；电脑的 IPv6 前缀变化后，设备需要回家连一次才能拿到新地址"});
    else
        out.push_back({Tone::Positive, L"域名",
                       endpoint_text(settings.public_hostname,
                                     settings.external_port ? settings.external_port : r.port) +
                           L"（DNS 记录由你的动态域名服务更新）"});
    // Protection counters (running host only; never credentials).
    if (host && settings.internet_access)
        out.push_back({Tone::Muted, L"公网防护",
                       L"已拒绝 " + std::to_wstring(stats.rejected) + L" 次 · 限流 " +
                           std::to_wstring(stats.throttled) + L" 次 · 握手超时 " + std::to_wstring(stats.timeouts) +
                           L" 次"});
    return out;
}
/** Every address the PC has, and whether devices are told about it. */
void fill_addresses() {
    auto list = c(77);
    ListView_DeleteAllItems(list);
    if (!report)
        return;
    int row = 0;
    for (auto& a : report->addresses) {
        const wchar_t* kind = a.category == AddressKind::private_v4 ? L"局域网 IPv4"
                              : a.category == AddressKind::ula_v6   ? L"局域网 IPv6"
                              : a.category == AddressKind::global_v6 ? L"公网 IPv6"
                              : a.category == AddressKind::public_v4 ? L"公网 IPv4"
                              : a.category == AddressKind::cgnat_v4  ? L"运营商 NAT"
                                                                      : L"其他";
        bool local = a.category == AddressKind::private_v4 || a.category == AddressKind::ula_v6;
        const wchar_t* use = a.deprecated  ? L"已弃用"
                             : a.temporary ? L"可能临时，不使用"
                             : a.category == AddressKind::cgnat_v4 ? L"不使用"
                             : !local && !settings.internet_access ? L"公网未开启"
                             : a.virtual_adapter                   ? L"使用中（虚拟网卡）"
                                                                   : L"使用中";
        cell(list, row, 0, display_host(a.address));
        cell(list, row, 1, kind);
        cell(list, row, 2, wide(a.adapter_name));
        cell(list, row, 3, use);
        ++row;
    }
}
void received(std::unique_ptr<ReachabilityReport> r) {
    checking = false;
    if (r)
        report = std::move(*r);
    if (host)
        stats = host->security_stats();
    fill_addresses();
    if (again) {
        again = false;
        refresh();
    }
    if (page == 3 || page == 2)
        layout();
}
/** Once a second on the Internet page: only the counters change without an event. */
void tick() {
    if (!host || !settings.internet_access)
        return;
    auto next = host->security_stats();
    if (next.rejected != stats.rejected || next.throttled != stats.throttled || next.timeouts != stats.timeouts) {
        stats = next;
        if (page == 3)
            layout();
    }
}
void shutdown() {
    join_probe();
    if (elevate.joinable())
        elevate.join();
}
} // namespace internet

struct SetupRoot {
    fs::path root;
    std::wstring title, ok; // empty = the resource's own “新建主机配置 / 下一步”
};
/** Opens a folder picker positioned at `start` when it exists (otherwise the system default). */
std::optional<fs::path> pick_folder(HWND owner, const fs::path& start, const wchar_t* title) {
    Microsoft::WRL::ComPtr<IFileOpenDialog> picker;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&picker))))
        throw Error(Code::unsupported, "目录选择器不可用");
    picker->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    picker->SetTitle(title);
    std::error_code ec;
    if (!start.empty() && fs::is_directory(start, ec)) {
        Microsoft::WRL::ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&folder))))
            picker->SetFolder(folder.Get()); // SetFolder (not SetDefaultFolder) overrides the last-used folder
    }
    if (FAILED(picker->Show(owner)))
        return std::nullopt;
    Microsoft::WRL::ComPtr<IShellItem> item;
    if (FAILED(picker->GetResult(&item)) || !item)
        throw Error(Code::io, "目录选择失败");
    PWSTR value = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &value)) || !value)
        throw Error(Code::io, "目录路径不可用");
    fs::path result(value);
    CoTaskMemFree(value);
    return result;
}
INT_PTR CALLBACK setup_proc(HWND dialog, UINT message, WPARAM w, LPARAM l) {
    auto data = reinterpret_cast<SetupRoot*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        data = reinterpret_cast<SetupRoot*>(l);
        SetWindowLongPtrW(dialog, DWLP_USER, l);
        SetDlgItemTextW(dialog, 700, data->root.c_str());
        if (!data->title.empty()) {
            SetWindowTextW(dialog, data->title.c_str());
            // The resource's footnote describes creating an identity; replace it in change mode.
            for (HWND child = GetWindow(dialog, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
                if (text(child).starts_with(L"设备身份"))
                    SetWindowTextW(child, L"证书指纹、已配对设备和密钥保持不变；保存后重新启动主机生效。");
        }
        if (!data->ok.empty())
            SetDlgItemTextW(dialog, IDOK, data->ok.c_str());
        return TRUE;
    }
    if (message == WM_COMMAND && LOWORD(w) == IDCANCEL) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    if (message != WM_COMMAND || !data)
        return FALSE;
    try {
        if (LOWORD(w) == 701) {
            if (auto chosen = pick_folder(dialog, fs::path(text(GetDlgItem(dialog, 700))).make_preferred(),
                                          L"选择明确授权给远程设备的目录"))
                SetDlgItemTextW(dialog, 700, chosen->c_str());
            return TRUE;
        }
        if (LOWORD(w) == IDOK) {
            auto root = text(GetDlgItem(dialog, 700));
            if (root.empty() || !fs::path(root).is_absolute() || !fs::is_directory(root))
                throw Error(Code::invalid, "请填写存在的绝对目录路径");
            data->root = fs::path(root);
            EndDialog(dialog, IDOK);
            return TRUE;
        }
    } catch (const std::exception& error) {
        MessageBoxW(dialog, wide(error.what()).c_str(), L"目录设置失败", MB_OK | MB_ICONWARNING);
    }
    return FALSE;
}
void command(int id) {
    if (id == 35) {
        if (host)
            throw Error(Code::busy, "先停止主机");
        SetupRoot setup{snapshot.root};
        if (DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), window, setup_proc,
                            reinterpret_cast<LPARAM>(&setup)) != IDOK)
            return;
        auto shared = setup.root;
        fs::path target = text(c(10));
        // An existing state folder can never be re-initialised, so a new host always gets its own folder.
        bool replaced = !target.empty() && fs::exists(target);
        if (target.empty() || replaced) {
            PWSTR local = nullptr;
            if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
                throw Error(Code::io, "LocalAppData不可用");
            target = fs::path(local) / L"Lantern" / L"hosts" / (L"host-" + wide(random_token().substr(0, 8)));
            CoTaskMemFree(local);
        }
        auto allowed = scopes(100);
        if (!allowed)
            throw Error(Code::invalid, "请先选择至少一项主机权限上限");
        auto prompt = L"新建主机身份：" + target.wstring() + L"\n授权目录：" + shared.wstring() +
                      L"\n权限上限：" + scope_text(allowed) + L"\n仅创建本机配置，不启动监听。继续？";
        if (replaced)
            prompt += L"\n\n这会创建一台新主机：证书指纹和设备密钥都是新的，手机需要重新配对。"
                      L"\n如果只想更换授权目录，请取消并使用“更改授权目录”。";
        if (MessageBoxW(window, prompt.c_str(), L"创建新主机", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
        try {
            HostAdministration::initialize(target, shared, allowed);
        } catch (const std::exception& e) {
            MessageBoxW(window, wide(e.what()).c_str(), L"未能创建主机", MB_OK | MB_ICONWARNING);
            throw;
        }
        SetWindowTextW(c(10), target.c_str());
        SetWindowTextW(c(13), wide(default_host_bind).c_str()); // a new host is meant for the LAN
        load();
        refresh_network_note();
        message(L"新身份已创建，旧共享凭据默认关闭。添加设备后再明确启动主机。");
    } else if (id == 38) {
        if (host)
            throw Error(Code::busy, "请先停止主机，再更改授权目录");
        if (!admin)
            throw Error(Code::invalid, "请先加载主机配置");
        SetupRoot setup{snapshot.root, L"更改授权目录", L"保存"};
        if (DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), window, setup_proc,
                            reinterpret_cast<LPARAM>(&setup)) != IDOK ||
            setup.root == snapshot.root)
            return;
        auto prompt = L"授权目录将从\n" + snapshot.root.wstring() + L"\n改为\n" + setup.root.wstring() +
                      L"\n\n证书指纹、已配对设备和密钥都保持不变；重新启动主机后生效。继续？";
        if (MessageBoxW(window, prompt.c_str(), L"更改授权目录", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
        try {
            admin->root(setup.root);
        } catch (const std::exception& e) {
            MessageBoxW(window, wide(e.what()).c_str(), L"未能更改授权目录", MB_OK | MB_ICONWARNING);
            throw;
        }
        refresh_devices();
        message(L"授权目录已更改为 " + snapshot.root.wstring() + L"；启动主机后手机即可访问。");
    } else if (id == 37) {
        if (!host)
            throw Error(Code::invalid, "先启动主机");
        auto value = utf8(text(c(36)));
        size_t used = 0;
        auto mbps = std::stoul(value, &used);
        if (used != value.size() || mbps < 1 || mbps > 8000)
            throw Error(Code::invalid, "带宽上限范围1..8000 Mbps");
        host->set_link_rate(uint64_t(mbps) * 125000);
        save_settings();
        message(L"共享链路上限已更新；控制/音频优先，文件利用剩余额度。该值需按实测可用带宽设置。");
    } else if (id == 11) {
        wchar_t name[32768]{};
        // Start where the state field points: prefill its config.dpapi so the dialog opens in that folder.
        // The common dialog rejects '/' in lpstrFile (it then refuses to open), so use native separators.
        fs::path current = fs::path(text(c(10))).make_preferred();
        std::error_code ec;
        if (!current.empty() && fs::is_directory(current, ec))
            wcsncpy_s(name, (current / L"config.dpapi").c_str(), _TRUNCATE);
        OPENFILENAMEW o{sizeof(o)};
        o.hwndOwner = window;
        o.lpstrFile = name;
        o.nMaxFile = 32768;
        o.lpstrFilter = L"Lantern配置\0config.dpapi\0";
        o.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&o))
            SetWindowTextW(c(10), fs::path(name).parent_path().c_str());
    } else if (id == 12)
        load();
    else if (id == 60 || id == 62)
        pairing::open();
    else if (id == 63) {
        message(L"正在请求管理员权限，为“专用网络”添加防火墙规则…");
        check_firewall(true);
    }
    else if (id >= 50 && id <= 53) {
        page = id - 50;
        layout();
        refresh_sessions();
        if (page == 3)
            internet::refresh();
    } else if (id == 70)
        internet::toggle();
    else if (id == 73)
        internet::save_fields();
    else if (id == 74)
        internet::request_firewall(true);
    else if (id == 75)
        internet::router(!internet::router_active());
    else if (id == 78) {
        internet::show_addresses = !internet::show_addresses;
        layout();
    } else if (id == 76) {
        address_fingerprint = host_address_fingerprint();
        refresh_network_note();
        internet::refresh();
    } else if (id == 15) {
        if (!admin)
            throw Error(Code::invalid, "请先加载主机状态目录");
        auto value = utf8(text(c(14)));
        size_t used = 0;
        auto port = std::stoul(value, &used);
        if (used != value.size() || !port || port > 65535)
            throw Error(Code::invalid, "端口必须为1..65535");
        host = std::make_unique<Host>(state, utf8(text(c(13))), uint16_t(port));
        // The real listener: “::” falls back to IPv4 when the system cannot open a dual-stack socket.
        listen_bind = host->listening_address();
        listen_port = host->listening_port();
        listen_address = endpoint_text(listen_bind, listen_port);
        auto capacity = utf8(text(c(36)));
        size_t capacity_used = 0;
        auto mbps = std::stoul(capacity, &capacity_used);
        if (capacity_used != capacity.size() || mbps < 1 || mbps > 8000) {
            host.reset();
            throw Error(Code::invalid, "带宽上限范围1..8000 Mbps");
        }
        host->set_link_rate(uint64_t(mbps) * 125000);
        server = std::thread([] {
            host->run();
            PostMessageW(window, WM_APP + 1, 0, 0);
        });
        save_settings();
        message(loopback_bind(listen_bind)
                    ? L"主机已启动，但只允许本机连接；要让手机连接，请停止后把监听地址改为 ::。"
                : listen_bind != utf8(text(c(13)))
                    ? L"主机已启动，但系统未能启用 IPv6，正在只用 IPv4 监听。局域网连接不受影响。"
                    : L"主机已启动。点“扫码添加设备”，用手机扫一下就能连上。" + wide(host->discovery_status()));
        refresh_network_note();
        check_firewall(false);
        pairing::host_changed();
        internet::refresh();
        layout();
    } else if (id == 16)
        stop();
    else if (id == 34) {
        if (!admin)
            throw Error(Code::invalid, "先加载主机配置");
        if (MessageBoxW(window, L"禁用旧共享凭据会结束仍使用旧密钥的连接。继续？", L"禁用旧共享凭据",
                        MB_YESNO | MB_ICONWARNING) == IDYES) {
            admin->disable_legacy();
            refresh_devices();
            internet::refresh();
            message(L"旧共享凭据已禁用，逐设备信任保留。");
        }
    } else if (id == 28) {
        bool show = SendMessageW(c(28), BM_GETCHECK, 0, 0) == BST_CHECKED;
        SendMessageW(c(27), EM_SETPASSWORDCHAR, show ? 0 : L'●', 0);
        InvalidateRect(c(27), nullptr, TRUE);
    } else if (id == 41) {
        auto i = selected(c(40));
        if (!host || i < 0 || size_t(i) >= sessions.size())
            throw Error(Code::invalid, "请选择当前连接");
        if (!host->end_session(sessions[i].id))
            throw Error(Code::not_found, "连接已经结束");
        message(L"连接已终止；新版手机端会显示“已断开”且不再自动重连。配对仍有效，需持续禁止时撤销设备。");
    } else if (id >= 17 && id <= 30) {
        if (!admin)
            throw Error(Code::invalid, "先选择现有主机状态目录并加载");
        if (id == 17)
            admin->ceiling(scopes(100));
        else if (id == 22) {
            auto d = admin->add(utf8(text(c(21))), scopes(200));
            refresh_devices();
            SetWindowTextW(c(27), wide(d.secret).c_str());
            message(L"新设备已创建；密钥仅在本窗口显示，默认遮罩。请核对指纹后配对。");
            return;
        } else if (id == 23)
            admin->grant(device_id(), scopes(200));
        else if (id == 24) {
            auto target = device_id();
            if (MessageBoxW(window, L"撤销后该设备不能再连接，现有连接也会终止。继续？", L"撤销设备信任",
                            MB_YESNO | MB_ICONWARNING) != IDYES)
                return;
            admin->remove(target);
        } else if (id == 25) {
            admin->end_device(device_id());
            refresh_devices();
            message(L"已结束该设备的全部连接；新版手机端会显示“已断开”且不再自动重连。配对仍有效，"
                    L"需持续禁止时撤销设备。");
            return;
        } else if (id == 26) {
            auto d = admin->pairing(device_id());
            hide_secret();
            SetWindowTextW(c(27), wide(d.secret).c_str());
            message(L"配对密钥已加载并遮罩；仅明确勾选后显示，不自动写入剪贴板。");
            return;
        } else if (id == 30) {
        } else
            return;
        refresh_devices();
        message(L"主机授权已保存；现有受影响连接会被终止。结束设备会话保留信任，撤销则持续禁止。");
    }
}
LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    try {
        switch (msg) {
        case WM_CREATE: {
            window = h;
            ui::dpi = GetDpiForWindow(h);
            ui::make_fonts();
            using ui::Kind;
            namespace g = ui::glyph;
            // Rail.
            const std::pair<int, const wchar_t*> navs[] = {
                {52, L"主机设置"}, {50, L"设备与权限"}, {51, L"当前连接"}, {53, L"公网访问"}};
            const wchar_t icons[] = {g::settings, g::people, g::plug, globe_glyph};
            for (int i = 0; i < 4; ++i) {
                auto nav = button(navs[i].first, -1, Kind::Nav, navs[i].second, icons[i], ui::Surface::Rail);
                int target = navs[i].first - 50;
                ui::item(nav)->on = [target] { return page == target; };
            }
            status = child(31, -1, L"STATIC", L"未加载配置。", SS_CENTERIMAGE | SS_ENDELLIPSIS | SS_NOPREFIX);
            // Host settings.
            child(10, 2, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP);
            SendMessageW(c(10), EM_SETCUEBANNER, TRUE, (LPARAM)L"主机状态目录（包含 config.dpapi 与 identity.dpapi）");
            button(11, 2, Kind::Secondary, L"浏览…", g::folder);
            button(12, 2, Kind::Secondary, L"加载", g::open);
            button(35, 2, Kind::Primary, L"新建主机配置", g::add);
            button(38, 2, Kind::Secondary, L"更改授权目录…", g::folder);
            child(32, 2, L"EDIT",
                  L"选择现有主机状态目录（含config.dpapi和identity."
                  L"dpapi）。\r\n不会自动修改授权目录或启动网络监听。",
                  ES_MULTILINE | ES_AUTOHSCROLL | ES_READONLY | WS_TABSTOP);
            const wchar_t* labels[] = {L"文件读取", L"文件写入", L"观看桌面", L"键鼠控制", L"系统音频"};
            for (int i = 0; i < 5; ++i)
                button(100 + i, 2, Kind::Chip, labels[i]);
            button(17, 2, Kind::Secondary, L"保存权限上限", g::save);
            // New setups listen on the LAN; a remembered address (even loopback) replaces this in restore.
            child(13, 2, L"EDIT", wide(default_host_bind).c_str(), ES_AUTOHSCROLL | WS_TABSTOP);
            child(14, 2, L"EDIT", L"47443", ES_NUMBER | WS_TABSTOP);
            child(36, 2, L"EDIT", L"320", ES_NUMBER | WS_TABSTOP);
            button(37, 2, Kind::Secondary, L"应用带宽上限");
            button(15, 2, Kind::Primary, L"启动主机", g::play);
            button(16, 2, Kind::Danger, L"停止主机", g::stop);
            button(62, 2, Kind::Secondary, L"扫码添加设备", phone_glyph);
            button(63, 2, Kind::Secondary, L"允许通过防火墙", g::shield);
            // Devices and permissions.
            child(19, 0, L"STATIC", L"旧共享凭据状态将在加载后显示。", SS_NOTIFY | SS_CENTERIMAGE | SS_ENDELLIPSIS);
            button(34, 0, Kind::Danger, L"禁用旧共享凭据", g::shield);
            auto list = child(20, 0, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP);
            ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            column(list, 0, L"设备名称", 200);
            column(list, 1, L"生效权限", 260);
            column(list, 2, L"设备 ID", 500);
            child(21, 0, L"EDIT", L"新设备", ES_AUTOHSCROLL | WS_TABSTOP);
            button(22, 0, Kind::Secondary, L"新增设备", g::add);
            button(60, 0, Kind::Primary, L"扫码添加", phone_glyph);
            for (int i = 0; i < 5; ++i)
                button(200 + i, 0, Kind::Chip, labels[i]);
            button(23, 0, Kind::Secondary, L"保存设备权限", g::save);
            button(26, 0, Kind::Secondary, L"查看配对资料", g::key);
            button(25, 0, Kind::Secondary, L"结束设备全部会话", g::cancel);
            button(24, 0, Kind::Danger, L"撤销设备", g::remove);
            button(30, 0, Kind::Ghost, L"刷新", g::refresh);
            child(27, 0, L"EDIT", L"", ES_READONLY | ES_PASSWORD | ES_AUTOHSCROLL | WS_TABSTOP);
            SendMessageW(c(27), EM_SETPASSWORDCHAR, L'●', 0);
            SendMessageW(c(27), EM_SETCUEBANNER, TRUE, (LPARAM)L"选中设备后点击“查看配对资料”");
            button(28, 0, Kind::Chip, L"显示配对密钥");
            child(33, 0, L"STATIC", L"勾选项是设备配置，与主机上限取交集后生效。配对密钥默认遮罩，不自动写入剪贴板。",
                  SS_NOPREFIX | SS_ENDELLIPSIS);
            // Live sessions.
            child(43, 1, L"STATIC",
                  L"终止连接后，新版手机端会显示“已断开”并停止自动重连；配对仍有效，需要持续禁止时请撤销设备。\n"
                  L"只有此窗口启动的主机才显示实时连接。",
                  SS_NOPREFIX);
            list = child(40, 1, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP);
            ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            column(list, 0, L"设备", 160);
            column(list, 1, L"来源地址", 150);
            column(list, 2, L"逻辑通道", 110);
            column(list, 3, L"请求权限", 220);
            column(list, 4, L"连接 ID", 450);
            button(41, 1, Kind::Danger, L"终止选中的连接", g::cancel, ui::Surface::Bg);
            // Internet access.
            button(70, 3, Kind::Primary, L"开启公网访问", globe_glyph);
            child(71, 3, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP);
            SendMessageW(c(71), EM_SETCUEBANNER, TRUE, (LPARAM)L"例如 home.example.com");
            child(72, 3, L"EDIT", L"", ES_NUMBER | WS_TABSTOP);
            SendMessageW(c(72), EM_SETCUEBANNER, TRUE, (LPARAM)L"同监听端口");
            button(73, 3, Kind::Secondary, L"保存", g::save);
            button(74, 3, Kind::Secondary, L"添加公网防火墙规则", g::shield);
            button(75, 3, Kind::Secondary, L"让路由器自动放行", g::sync);
            button(76, 3, Kind::Ghost, L"重新检查", g::refresh);
            button(78, 3, Kind::Ghost, L"全部地址", g::view);
            list = child(77, 3, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP);
            ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            column(list, 0, L"地址", 300);
            column(list, 1, L"类型", 110);
            column(list, 2, L"网卡", 170);
            column(list, 3, L"状态", 150);
            scopes(200, 1);
            scopes(100, 1);
            restyle();
            SetTimer(h, 1, 1000, nullptr);
            layout();
            PostMessageW(h, WM_APP + 2, 0, 0); // restore once the window exists
            return 0;
        }
        case WM_APP + 2:
            restore_settings();
            address_fingerprint = host_address_fingerprint();
            refresh_network_note();
            check_firewall(false);
            layout();
            refresh_rail();
            return 0;
        case internet::report_msg:
            internet::received(std::unique_ptr<ReachabilityReport>(reinterpret_cast<ReachabilityReport*>(l)));
            return 0;
        case internet::action_msg: {
            std::unique_ptr<internet::Done> done(reinterpret_cast<internet::Done*>(l));
            if (!closing)
                internet::finish(*done);
            return 0;
        }
        case firewall_msg:
            firewall = FirewallState(w);
            firewall_known = true;
            if (l == 2)
                message(L"没有添加防火墙规则（未获得管理员授权或操作失败）。也可以在 Windows 安全中心手动允许 lantern-host.exe。");
            else if (l == 1)
                message(firewall == FirewallState::allowed ? L"已允许 Lantern 在“专用网络”中接受连接；公用网络仍然拦截。"
                                                           : L"防火墙规则已提交，但状态仍无法确认。");
            refresh_network_note();
            layout();
            return 0;
        case WM_SIZE:
            if (!controls.empty())
                layout();
            return 0;
        case WM_GETMINMAXINFO:
            ((MINMAXINFO*)l)->ptMinTrackSize = {ui::dp(980), ui::dp(720)};
            return 0;
        case WM_PAINT:
            ui::paint(h, scene, paint_rail);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_DRAWITEM:
            ui::draw((DRAWITEMSTRUCT*)l);
            return TRUE;
        case WM_DPICHANGED: {
            ui::dpi = HIWORD(w);
            ui::make_fonts();
            for (auto [id, handle] : controls)
                SendMessageW(handle, WM_SETFONT, (WPARAM)ui::fonts.body, TRUE);
            restyle();
            ui::window_icons(h);
            auto r = (RECT*)l;
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            layout();
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (l && wcscmp((const wchar_t*)l, L"ImmersiveColorSet") == 0) {
                ui::load_palette();
                restyle();
                layout();
                RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
            }
            return 0;
        case WM_COMMAND: {
            if (closing)
                return 0;
            auto notice = HIWORD(w);
            if (l && (notice == EN_SETFOCUS || notice == EN_KILLFOCUS)) {
                ui::invalidate_field(window, scene, (HWND)l);
                return 0;
            }
            if (notice == EN_CHANGE && (LOWORD(w) == 13 || LOWORD(w) == 14)) {
                refresh_network_note();
                return 0;
            }
            if (notice == BN_CLICKED || (l && notice == BN_DOUBLECLICKED && ui::item((HWND)l))) {
                if (l)
                    ui::clicked((HWND)l);
                command(LOWORD(w));
                refresh_rail();
            }
            return 0;
        }
        case WM_NOTIFY: {
            auto hdr = (NMHDR*)l;
            if (hdr->code == LVN_GETEMPTYMARKUP && (hdr->idFrom == 20 || hdr->idFrom == 40)) {
                auto markup = (NMLVEMPTYMARKUP*)l;
                markup->dwFlags = EMF_CENTERED;
                wcsncpy_s(markup->szMarkup,
                          hdr->idFrom == 20 ? (admin ? ui_text::devices_empty : ui_text::devices_unloaded)
                                            : (host ? ui_text::sessions_empty : ui_text::sessions_stopped),
                          _TRUNCATE);
                return TRUE;
            }
            if (hdr->idFrom == 20 && hdr->code == LVN_ITEMCHANGED) {
                int i = selected(c(20));
                if (i >= 0 && size_t(i) < snapshot.devices.size()) {
                    scopes(200, snapshot.devices[i].scopes);
                    hide_secret();
                }
            }
            return 0;
        }
        case WM_TIMER:
            if (!closing) {
                auto before = sessions.size();
                refresh_sessions();
                if (before != sessions.size())
                    refresh_rail();
                static int ticks = 0;
                if (++ticks % 3 == 0) {
                    // A new IPv6 prefix or another Wi-Fi: refresh everything that names an address.
                    uint64_t now = address_fingerprint;
                    try {
                        now = host_address_fingerprint();
                    } catch (...) {
                    }
                    if (now != address_fingerprint) {
                        address_fingerprint = now;
                        refresh_network_note();
                        pairing::addresses_changed(now);
                        internet::addresses_changed();
                    }
                }
                internet::tick();
            }
            return 0;
        case WM_APP + 1:
            if (server.joinable())
                server.join();
            if (stopper.joinable())
                stopper.join();
            internet::join_probe(); // it may hold the Host pointer
            host.reset();
            stopping = false;
            internet::router_wanted = false;
            internet::busy = false;
            refresh_sessions();
            pairing::host_changed();
            refresh_network_note();
            if (!closing)
                internet::refresh();
            if (closing)
                DestroyWindow(h);
            else {
                message(L"主机已停止，所有连接已结束。");
                layout();
                refresh_rail();
            }
            return 0;
        case WM_CLOSE:
            if (closing)
                return 0;
            closing = true;
            KillTimer(h, 1);
            hide_secret();
            pairing::close();
            save_settings();
            if (host) {
                stop();
                return 0;
            }
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            if (firewall_worker.joinable())
                firewall_worker.join();
            internet::shutdown();
            PostQuitMessage(0);
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            auto child_window = (HWND)l;
            int id = GetDlgCtrlID(child_window);
            COLORREF under = ui::under(scene, child_window);
            bool quiet = id == 31 || id == 33 || id == 43;
            SetTextColor((HDC)w, quiet ? ui::pal.muted : ui::pal.ink);
            SetBkColor((HDC)w, under);
            return (LRESULT)ui::brush(under);
        }
        }
    } catch (const std::exception& e) {
        if (status)
            message(wide(e.what()));
    }
    return DefWindowProcW(h, msg, w, l);
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&common);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ui::startup();
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.lpszClassName = L"LanternHostManager";
    wc.hbrBackground = nullptr;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    ui::class_icons(wc);
    RegisterClassExW(&wc);
    auto h = CreateWindowW(wc.lpszClassName, L"Lantern 主机管理", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                           CW_USEDEFAULT, CW_USEDEFAULT, 1150, 900, nullptr, nullptr, instance, nullptr);
    if (!h)
        return 1;
    ui::window_icons(h);
    ui::initial_size(h, 1180, 860);
    ShowWindow(h, show);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        if (!(pairing::w && IsDialogMessageW(pairing::w, &msg)) && !IsDialogMessageW(h, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    ui::shutdown();
    CoUninitialize();
    return 0;
}
