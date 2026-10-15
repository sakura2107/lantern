#include "lantern/discovery.hpp"
#include "lantern/pairing.hpp"
#include "lantern/pairing_client.hpp"
#include "lantern/windows_client.hpp"
#include "lantern/windows_clipboard.hpp"
#include "lantern/windows_media.hpp"
#include "lantern/windows_playback.hpp"
#include "lantern/windows_thumbnail.hpp"
#include "ui_kit.hpp"
#include <commctrl.h>
#include <commdlg.h>
#include <deque>
#include <shellapi.h>
#include <shlobj.h>
#include <sstream>
#include <wincodec.h>
#include <wrl/client.h>
using namespace lantern;
using Microsoft::WRL::ComPtr;
namespace {
constexpr UINT WM_UPDATE = WM_APP + 1, WM_IMAGE = WM_APP + 2, WM_TASKS = WM_APP + 3,
               WM_INPUT_FAILURE = WM_APP + 4, WM_SHUTDOWN = WM_APP + 5, WM_THUMB = WM_APP + 6;
HWND window, desktop, statusbox;
std::map<int, HWND> controls;
/** Page that owns a control: -1 = every normal page, 0..3 = one page, or one of the fullscreen roles. */
std::map<int, int> pages;
constexpr int ALL_PAGES = -1, FULL_ONLY = 9, CHROME_HANDLE = 8;
/** Painted presentation (rail, cards, field frames, headings), rebuilt by layout(). */
ui::Scene scene;
/** Connection chip on the rail: 0 unpaired, 1 saved, 2 verified, 3 failed, 4 verifying. */
int connection_state = 0;
bool chrome_hidden = false;

// Presentation strings. Business/status messages stay next to the code that produces them.
namespace ui_text {
const wchar_t* const titles[] = {L"设备", L"远程桌面", L"电脑文件", L"传输"};
const wchar_t* const subtitles[] = {L"连接并核对要访问的电脑", L"画面、控制与声音分别开启，互不影响",
                                    L"无需开启画面即可浏览授权目录", L"上传、下载与任务策略；退出后进度保留"};
const wchar_t* const connection[] = {L"未配对", L"已保存，尚未验证", L"已验证", L"验证失败", L"正在验证…"};
constexpr const wchar_t* connect_title = L"连接电脑";
constexpr const wchar_t* connect_hint = L"首次连接请核对电脑主机管理中显示的证书指纹；局域网发现只填写地址，不代表信任。";
constexpr const wchar_t* privacy_title = L"权限与隐私";
constexpr const wchar_t* privacy =
    L"观看、控制、声音、文件读取和写入分别授权，由电脑上的主机管理决定。\n配对密钥使用当前 Windows 用户的 "
    L"DPAPI 加密保存，不写入日志。\n“隐藏本机画面”只是不在本机显示，电脑仍在采集和传输；“画面”关闭才会结束"
    L"桌面连接。电脑端隐私屏暂不支持。";
constexpr const wchar_t* preview_title = L"预览";
constexpr const wchar_t* preview_idle = L"选择 JPG、PNG 或 BMP 图片查看预览。";
constexpr const wchar_t* files_empty = L"这里还没有内容。输入目录后点击刷新，留空表示授权根目录。";
constexpr const wchar_t* files_unpaired = L"还没有连接电脑。请先在“设备”页验证。";
constexpr const wchar_t* tasks_empty = L"还没有传输任务。在“文件”页上传或下载后会显示在这里。";
constexpr const wchar_t* tasks_note =
    L"最多保留 32 项任务，单线程传输并做 SHA-256 校验；退出后保留进度，重新打开后手动继续。"
    L"同名冲突时用“另存为”选择新目的地，不会覆盖原文件。";
constexpr const wchar_t* desktop_off = L"画面未开启";
constexpr const wchar_t* desktop_off_hint = L"点击上方“画面”开始接收电脑桌面";
constexpr const wchar_t* desktop_hidden = L"本机画面已隐藏";
constexpr const wchar_t* desktop_hidden_hint = L"电脑仍在采集和传输；关闭“隐藏本机画面”即可恢复";
constexpr const wchar_t* desktop_waiting = L"正在连接画面…";
} // namespace ui_text
std::mutex credentials_mutex, work_mutex, updates_mutex, input_mutex;
std::condition_variable work_cv, input_cv;
ClientCredentials credentials;
std::shared_ptr<AvTimeline> av_timeline = std::make_shared<AvTimeline>();
std::shared_ptr<AvTimeline> timeline_snapshot() {
    std::lock_guard lock(credentials_mutex);
    return av_timeline;
}
fs::path state_dir;
struct Update {
    std::wstring status;
    DirectoryPage page;
    std::string path;
    uint32_t offset = 0;
    bool listing = false;
    unsigned epoch = 0;
    fs::path open;
    bool chooser = false;
    bool discovery_result = false;
    std::vector<DiscoveredDevice> discovered;
    int connection = -1;
    std::optional<ClientCredentials> paired; // fills the device fields after link pairing
};
std::deque<Update> updates;
bool update_posted = false;
std::deque<std::function<void()>> work;
struct InputEvent {
    Op op;
    Writer body;
    bool move = false;
    unsigned epoch = 0;
};
std::deque<InputEvent> input_queue;
std::thread work_thread, video_thread, audio_thread, input_thread;
std::unique_ptr<WindowsTransfers> transfers;
std::unique_ptr<WindowsMediaBridge> media_bridge;
std::mutex media_mutex;
std::unique_ptr<ClipboardSync> clipboard_sync;
std::unique_ptr<ThumbnailLoader> thumbnails;
std::unique_ptr<ThumbnailResult> pending_thumbnail;
std::atomic_bool media_active = false;
HBITMAP thumbnail_bitmap = nullptr;
uint64_t thumbnail_ticket = 0;
std::string last_preview_path, policy_selected;
bool replacing_tasks = false;
std::thread shutdown_thread;
ActiveConnection video_connection, audio_connection, input_connection, file_connection;
std::atomic_bool closing = false, subscribed = false, audio_on = false, show_picture = true,
                 control_on = false, gpu_picture = false, visible_desktop = false;
std::atomic_uint video_epoch = 0, audio_epoch = 0, input_epoch = 0, monitor_index = 0, source_w = 1920,
                 source_h = 1080;
std::atomic_uint target_width = 1920, target_fps = 60, target_bitrate = 12'000'000;
std::atomic<double> zoom = 1;
Cancel file_cancel, audio_cancel;
std::vector<Entry> entries;
std::vector<TransferTask> task_snapshot;
std::string listing_path, generation;
uint32_t offset = 0, total = 0;
unsigned listing_epoch = 0;
int page = 0;
bool fullscreen = false, dragging = false;
RECT normal_rect{};
LONG_PTR normal_style = 0;
int pointer_x = 0, pointer_y = 0;
using Picture = PixelImage;
Picture picture;
std::unique_ptr<Picture> pending_picture;
bool picture_posted = false;
std::wstring text(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n + 1, 0);
    GetWindowTextW(h, s.data(), n + 1);
    s.resize(n);
    return s;
}
HWND ctl(int id) {
    return controls.at(id);
}
ClientCredentials creds() {
    std::lock_guard lock(credentials_mutex);
    return credentials;
}
void publish(Update u) {
    std::lock_guard lock(updates_mutex);
    if (updates.size() >= 32) {
        if (!u.listing && u.open.empty() && !u.discovery_result)
            return;
        throw Error(Code::busy, "UI result queue full");
    }
    updates.push_back(std::move(u));
    if (!update_posted)
        update_posted = PostMessageW(window, WM_UPDATE, 0, 0) != 0;
}
void status(std::wstring s) {
    Update u;
    u.status = std::move(s);
    publish(std::move(u));
}
void enqueue(std::function<void()> f) {
    std::lock_guard lock(work_mutex);
    if (work.size() >= 16)
        throw Error(Code::busy, "操作队列已满");
    work.push_back(std::move(f));
    work_cv.notify_one();
}
HWND child(int id, int p, const wchar_t* type, const wchar_t* label, DWORD style = 0) {
    auto h = CreateWindowExW(0, type, label, WS_CHILD | style, 0, 0, 10, 10, window, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(nullptr), nullptr);
    controls[id] = h;
    pages[id] = p;
    SendMessageW(h, WM_SETFONT, (WPARAM)ui::fonts.body, TRUE);
    return h;
}
/** Owner-drawn button registered like any other control. */
HWND button(int id, int p, ui::Kind kind, const wchar_t* label, wchar_t glyph = 0,
            ui::Surface surface = ui::Surface::Bg) {
    auto h = ui::button(window, id, kind, label, glyph, surface);
    controls[id] = h;
    pages[id] = p;
    return h;
}
HWND tile(int id, const wchar_t* label, wchar_t glyph, const wchar_t* on_text, const wchar_t* off_text,
          std::function<bool()> on) {
    auto h = button(id, 1, ui::Kind::Tile, label, glyph);
    auto item = ui::item(h);
    item->on = std::move(on);
    item->on_text = on_text;
    item->off_text = off_text;
    return h;
}
void move(int id, int x, int y, int w, int h) {
    MoveWindow(ctl(id), x, y, w, h, TRUE);
}
void place(int id, RECT r) {
    MoveWindow(ctl(id), r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
}
/** Drop-down lists size their own visible height; centre that inside the slot. */
void place_combo(int id, RECT slot) {
    auto h = ctl(id);
    MoveWindow(h, slot.left, slot.top, slot.right - slot.left, ui::dp(320), TRUE);
    RECT r{};
    GetWindowRect(h, &r);
    int visible = r.bottom - r.top;
    MoveWindow(h, slot.left, slot.top + (slot.bottom - slot.top - visible) / 2, slot.right - slot.left,
               ui::dp(320), TRUE);
}
std::wstring bytes(uint64_t n) {
    if (n < 1024)
        return std::to_wstring(n) + L" B";
    std::wostringstream s;
    s.precision(1);
    s << std::fixed << double(n) / (n >= 1024ull * 1024 * 1024 ? 1024. * 1024 * 1024 : 1024. * 1024)
      << (n >= 1024ull * 1024 * 1024 ? L" GiB" : L" MiB");
    return s.str();
}
void input(Op op, Writer w, bool move_event = false) {
    if (!control_on)
        return;
    std::lock_guard lock(input_mutex);
    if (move_event && !input_queue.empty() && input_queue.back().move) {
        input_queue.back().body = std::move(w);
        return;
    }
    if (input_queue.size() >= 64) {
        control_on = false;
        input_queue.clear();
        input_connection.interrupt();
        PostMessageW(window, WM_INPUT_FAILURE, 0, 0);
        return;
    }
    input_queue.push_back({op, std::move(w), move_event, input_epoch.load()});
    input_cv.notify_one();
}
void pointer(uint32_t phase, int32_t x, int32_t y) {
    Writer w;
    w.u32(phase);
    w.u32(uint32_t(x));
    w.u32(uint32_t(y));
    w.u32(monitor_index);
    input(Op::pointer_gesture, std::move(w), phase == 2);
}
void cancel_pointer() {
    if (dragging) {
        dragging = false;
        pointer(4, 0, 0);
        ReleaseCapture();
    }
    KillTimer(desktop, 1);
}
void release_control() {
    cancel_pointer();
    control_on = false;
    ++input_epoch;
    input_connection.interrupt();
    std::lock_guard lock(input_mutex);
    input_queue.clear();
}
constexpr int strip_ids[] = {16, 17, 20, 23, 25, 21, 74, 26, 31, 24, 117};
bool in_strip(int id) {
    return std::find(std::begin(strip_ids), std::end(strip_ids), id) != std::end(strip_ids);
}
bool more_pages() {
    return offset + entries.size() < total;
}
bool media_shown() {
    return controls.count(62) && GetWindowTextLengthW(ctl(62)) > 0;
}
int rail_width() {
    return ui::dp(212);
}
/** Columns follow the list width; the name column takes what is left. */
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
    auto visible = [](int id) {
        int p = pages[id];
        if (fullscreen)
            return chrome_hidden ? p == CHROME_HANDLE : in_strip(id);
        if (p == CHROME_HANDLE || p == FULL_ONLY)
            return false;
        if (p != ALL_PAGES && p != page)
            return false;
        if (id == 19)
            return more_pages();
        if (id == 62 || id == 53 || id == 54)
            return media_shown();
        return true;
    };
    for (auto [id, handle] : controls)
        ShowWindow(handle, visible(id) ? SW_SHOW : SW_HIDE);
    visible_desktop = page == 1;
    ShowWindow(desktop, page == 1 ? SW_SHOW : SW_HIDE);
    for (int id : strip_ids)
        ui::set_surface(ctl(id), fullscreen ? ui::Surface::Chrome : ui::Surface::Bg);
    ui::style_combo(ctl(25), fullscreen);

    if (fullscreen) {
        int strip = 0;
        if (!chrome_hidden) {
            int bh = dp(38), top = dp(9);
            int x = w - dp(12);
            for (int id : {117, 24, 31}) {
                int bw = ui::preferred_width(ctl(id), bh);
                x -= bw;
                move(id, x, top, bw, bh);
                x -= dp(8);
            }
            ui::Flow f(dp(12), x - dp(8), top, dp(8));
            for (int id : {16, 17, 20, 23})
                f.place(ctl(id), bh);
            place_combo(25, f.next(dp(160), bh));
            for (int id : {21, 74, 26})
                f.place(ctl(id), bh);
            strip = f.bottom() + top;
            scene.area({0, 0, w, strip}, ui::pal.chrome);
        } else {
            move(118, w / 2 - dp(36), 0, dp(72), dp(24));
            SetWindowPos(ctl(118), HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        }
        MoveWindow(desktop, 0, strip, w, std::max(1, h - strip), TRUE);
        InvalidateRect(window, nullptr, FALSE);
        return;
    }

    // Navigation rail.
    int rail = rail_width();
    scene.area({0, 0, rail, h}, ui::pal.rail);
    scene.hairlines.push_back({rail - 1, 0, rail, h});
    scene.dots.push_back({dp(30), dp(40), dp(6), ui::pal.lamp});
    scene.text({dp(46), dp(24), rail - dp(12), dp(56)}, L"Lantern", &ui::fonts.heading);
    for (int i = 0; i < 4; ++i)
        move(101 + i, dp(12), dp(84) + i * dp(46), rail - dp(24), dp(42));
    move(72, dp(12), h - dp(140), rail - dp(24), dp(40));

    // Content frame: page title, then page body, then the message line.
    int x0 = rail + dp(32), R = w - dp(32), y0 = dp(108), bottom = h - dp(60);
    scene.text({x0, dp(22), R, dp(60)}, ui_text::titles[page], &ui::fonts.title);
    scene.text({x0, dp(60), R, dp(84)}, ui_text::subtitles[page], &ui::fonts.body, ui::Tone::Muted);
    scene.hairlines.push_back({rail, h - dp(44), w, h - dp(43)});
    wchar_t info[2] = {ui::glyph::info, 0};
    scene.text({x0, h - dp(43), x0 + dp(20), h}, info, &ui::fonts.icon_small, ui::Tone::Muted,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    move(6, x0 + dp(26), h - dp(43), R - x0 - dp(26), dp(43));

    int bh = dp(36);
    if (page == 0) {
        int cw = std::min(R - x0, dp(780)), pad = dp(24), cx = x0 + pad, cr = x0 + cw - pad;
        int y = y0 + pad;
        scene.text({cx, y, cr, y + dp(24)}, ui_text::connect_title, &ui::fonts.heading);
        y += dp(28);
        scene.text({cx, y, cr, y + dp(20)}, ui_text::connect_hint, &ui::fonts.note, ui::Tone::Muted);
        y += dp(36);
        int lw = dp(116);
        for (int i = 0; i < 3; ++i) {
            move(111 + i, cx, y, lw - dp(8), dp(40));
            ui::field(scene, ctl(1 + i), {cx + lw, y, cr, y + dp(40)});
            y += dp(52);
        }
        ui::Flow f(cx + lw, cr, y + dp(4), dp(10));
        for (int id : {10, 73, 121, 22})
            f.place(ctl(id), dp(40));
        y = f.bottom() + pad;
        scene.card({x0, y0, x0 + cw, y});
        int top = y + dp(16);
        scene.text({cx, top + pad, cr, top + pad + dp(24)}, ui_text::privacy_title, &ui::fonts.heading);
        int ny = top + pad + dp(32), nh = std::clamp(bottom - ny - pad, dp(44), dp(112));
        move(114, cx, ny, cr - cx, nh);
        scene.card({x0, top, x0 + cw, ny + nh + pad});
    }
    if (page == 1) {
        int fw = ui::preferred_width(ctl(24), dp(38));
        move(24, R - fw, dp(30), fw, dp(38));
        ui::Flow f(x0, R, y0, dp(10));
        for (int id : {16, 17, 20, 23})
            f.place(ctl(id), dp(56));
        ui::Flow g(x0, R, f.bottom() + dp(12), dp(8));
        place_combo(25, g.next(dp(170), bh));
        for (int id : {21, 74, 26})
            g.place(ctl(id), bh);
        int y = g.bottom() + dp(14);
        // Keyboard and clipboard bar along the bottom; wraps to two rows on narrow windows.
        int keys = ui::row_width({ctl(28), ctl(29), ctl(30)}, bh, dp(8)) + dp(8) + dp(170);
        int clip = ui::row_width({ctl(63), ctl(64), ctl(65)}, bh, dp(8));
        bool one_row = dp(220) + dp(8) + keys + dp(24) + clip <= R - x0;
        int by = bottom - bh - (one_row ? 0 : bh + dp(10));
        int fieldw = (R - x0) - keys - dp(8) - (one_row ? clip + dp(24) : 0);
        ui::field(scene, ctl(27), {x0, by, x0 + fieldw, by + bh});
        ui::Flow k(x0 + fieldw + dp(8), R, by, dp(8));
        for (int id : {28, 29, 30})
            k.place(ctl(id), bh);
        place_combo(56, k.next(dp(170), bh));
        ui::Flow c(one_row ? R - clip : x0, R, one_row ? by : by + bh + dp(10), dp(8));
        for (int id : {63, 64, 65})
            c.place(ctl(id), bh);
        MoveWindow(desktop, x0, y, R - x0, std::max(1, by - dp(14) - y), TRUE);
    }
    if (page == 2) {
        int y = y0;
        move(12, x0, y, bh, bh);
        move(11, x0 + bh + dp(6), y, bh, bh);
        int sortw = dp(150);
        ui::field(scene, ctl(4), {x0 + 2 * bh + dp(16), y, R - sortw - dp(10), y + bh});
        place_combo(36, {R - sortw, y, R, y + bh});
        y += bh + dp(12);
        int search = ui::row_width({ctl(33), ctl(34)}, bh, dp(8)), upload = ui::row_width({ctl(14), ctl(55)}, bh, dp(8));
        int sw = std::clamp(R - x0 - search - upload - dp(40), dp(180), dp(360));
        ui::field(scene, ctl(32), {x0, y, x0 + sw, y + bh});
        ui::Flow f(x0 + sw + dp(8), R, y, dp(8));
        f.place(ctl(33), bh);
        f.place(ctl(34), bh);
        bool same_row = f.y == y && f.x + dp(16) <= R - upload;
        ui::Flow u(same_row ? R - upload : x0, R, same_row ? y : f.bottom() + dp(10), dp(8));
        u.place(ctl(14), bh);
        u.place(ctl(55), bh);
        y = std::max(f.bottom(), u.bottom()) + dp(12);
        ui::Flow a(x0, R, y, dp(8));
        for (int id : {13, 15, 37, 119})
            a.place(ctl(id), bh);
        if (more_pages()) {
            int lw = ui::preferred_width(ctl(19), bh);
            move(19, R - lw, a.y, lw, bh);
        }
        y = a.bottom() + dp(14);
        int pw = dp(248), list_bottom = bottom - (media_shown() ? dp(50) : 0);
        RECT lc{x0, y, R - pw - dp(16), list_bottom};
        scene.card(lc);
        move(5, lc.left + dp(6), lc.top + dp(6), lc.right - lc.left - dp(12), lc.bottom - lc.top - dp(12));
        fit_columns(ctl(5), {80, 110, 170}, 0);
        RECT pc{R - pw, y, R, list_bottom};
        scene.card(pc);
        scene.text({pc.left + dp(18), pc.top + dp(14), pc.right - dp(18), pc.top + dp(40)}, ui_text::preview_title,
                   &ui::fonts.heading);
        RECT well{pc.left + dp(16), pc.top + dp(48), pc.right - dp(16), pc.top + dp(48) + dp(180)};
        scene.well(well);
        move(66, well.left + dp(4), well.top + dp(4), well.right - well.left - dp(8), well.bottom - well.top - dp(8));
        move(67, pc.left + dp(18), well.bottom + dp(12), pw - dp(36), std::max(dp(20), int(pc.bottom) - int(well.bottom) - dp(24)));
        if (media_shown()) {
            int my = bottom - bh;
            int tools = ui::row_width({ctl(53), ctl(54)}, bh, dp(8));
            ui::field(scene, ctl(62), {x0, my, R - tools - dp(8), my + bh});
            ui::Flow m(R - tools, R, my, dp(8));
            m.place(ctl(53), bh);
            m.place(ctl(54), bh);
        }
    }
    if (page == 3) {
        ui::Flow a(x0, R, y0, dp(8));
        for (int id : {46, 47, 48, 49, 50})
            a.place(ctl(id), bh);
        int lw = ui::text_width(ui::fonts.body, text(ctl(71))) + dp(12);
        int policy = lw + dp(150) + dp(8) + dp(130) + dp(8) + ui::preferred_width(ctl(70), bh);
        bool same_row = a.x + dp(16) <= R - policy;
        int px = same_row ? R - policy : x0, py = same_row ? y0 : a.bottom() + dp(10);
        move(71, px, py, lw, bh);
        place_combo(68, {px + lw, py, px + lw + dp(150), py + bh});
        place_combo(69, {px + lw + dp(158), py, px + lw + dp(288), py + bh});
        move(70, px + lw + dp(296), py, ui::preferred_width(ctl(70), bh), bh);
        int y = std::max(a.bottom(), py + bh) + dp(14);
        RECT lc{x0, y, R, bottom - dp(52)};
        scene.card(lc);
        move(45, lc.left + dp(6), lc.top + dp(6), lc.right - lc.left - dp(12), lc.bottom - lc.top - dp(12));
        fit_columns(ctl(45), {90, 190, 230, 76, 96}, 1);
        move(115, x0, bottom - dp(44), R - x0, dp(44));
    }
    InvalidateRect(window, nullptr, FALSE);
}
/** Repaints everything that mirrors live state (tiles, navigation, rail chip, empty desktop). */
void refresh_state() {
    for (int id : {16, 17, 20, 23, 101, 102, 103, 104})
        InvalidateRect(ctl(id), nullptr, FALSE);
    RECT rc{};
    GetClientRect(window, &rc);
    RECT chip{0, rc.bottom - ui::dp(96), rail_width(), rc.bottom};
    InvalidateRect(window, &chip, FALSE);
    if (!gpu_picture || !show_picture)
        InvalidateRect(desktop, nullptr, FALSE);
}
/** Connection chip at the foot of the rail. */
void paint_rail(HDC dc) {
    using ui::dp;
    if (fullscreen)
        return;
    RECT rc{};
    GetClientRect(window, &rc);
    RECT chip{dp(12), rc.bottom - dp(88), rail_width() - dp(12), rc.bottom - dp(16)};
    ui::round_rect(dc, chip, dp(12), ui::pal.surface, ui::pal.line);
    COLORREF dot = connection_state == 2   ? ui::pal.lamp
                   : connection_state == 3 ? ui::pal.danger
                   : connection_state == 4 ? ui::mix(ui::pal.lamp, ui::pal.surface, .5)
                                           : ui::pal.faint;
    ui::circle(dc, chip.left + dp(18), chip.top + dp(24), dp(4), dot);
    std::wstring state = ui_text::connection[connection_state];
    if (connection_state == 2 && (subscribed || audio_on))
        state += subscribed ? L" · 画面开启" : L" · 声音开启";
    ui::draw_text(dc, state, {chip.left + dp(30), chip.top + dp(12), chip.right - dp(10), chip.top + dp(36)},
                  ui::fonts.strong, ui::pal.ink);
    auto c = creds();
    std::wstring where = c.host.empty() ? L"填写地址、指纹与密钥" : wide(format_endpoint({c.host, c.port}));
    ui::draw_text(dc, where, {chip.left + dp(30), chip.top + dp(36), chip.right - dp(10), chip.bottom - dp(10)},
                  ui::fonts.note, ui::pal.muted);
}
/** Native controls that need palette/DPI-aware theming; called on create, DPI and theme changes. */
void restyle() {
    ui::frame(window);
    for (int id : {5, 45})
        ui::style_list(ctl(id));
    for (int id : {25, 36, 56, 68, 69})
        ui::style_combo(ctl(id));
}
void apply_fonts() {
    for (auto [id, handle] : controls)
        SendMessageW(handle, WM_SETFONT, (WPARAM)ui::fonts.body, TRUE);
}
void switch_page(int p) {
    if (page == 1 && p != 1)
        cancel_pointer();
    page = p;
    layout();
    refresh_state();
}
void toggle_fullscreen() {
    cancel_pointer();
    fullscreen = !fullscreen;
    chrome_hidden = false;
    SetWindowTextW(ctl(24), fullscreen ? L"退出全屏" : L"全屏");
    ui::item(ctl(24))->glyph = fullscreen ? ui::glyph::exit_full : ui::glyph::fullscreen;
    if (fullscreen) {
        GetWindowRect(window, &normal_rect);
        normal_style = GetWindowLongPtrW(window, GWL_STYLE);
        SetWindowLongPtrW(window, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowPos(window, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED);
        switch_page(1);
    } else {
        SetWindowLongPtrW(window, GWL_STYLE, normal_style);
        SetWindowPos(window, nullptr, normal_rect.left, normal_rect.top, normal_rect.right - normal_rect.left,
                     normal_rect.bottom - normal_rect.top, SWP_NOZORDER | SWP_FRAMECHANGED);
        layout();
    }
    refresh_state();
}
void refresh(uint32_t off = 0) {
    auto c = creds();
    auto p = utf8(text(ctl(4))), filter = utf8(text(ctl(32))), gen = off ? generation : std::string{};
    auto sort = uint32_t(SendMessageW(ctl(36), CB_GETCURSEL, 0, 0));
    unsigned e = ++listing_epoch;
    file_cancel.requested = true;
    file_connection.interrupt();
    enqueue([c, p, filter, gen, off, sort, e] {
        file_cancel.requested = false;
        auto client = c.connect();
        file_connection.set(client);
        Update u;
        u.page = client_page(*client, p, off, filter, sort % 3, sort >= 3, gen);
        u.path = p;
        u.offset = off;
        u.listing = true;
        u.epoch = e;
        u.status =
            L"文件 " + std::to_wstring(off + u.page.entries.size()) + L" / " + std::to_wstring(u.page.total);
        publish(std::move(u));
        file_connection.set({});
    });
}
std::vector<int> selected_indices(HWND list) {
    std::vector<int> indices;
    int i = -1;
    while ((i = ListView_GetNextItem(list, i, LVNI_SELECTED)) >= 0)
        indices.push_back(i);
    return indices;
}
std::string remote_at(int i) {
    if (i < 0 || size_t(i) >= entries.size())
        throw Error(Code::invalid, "请先选择文件");
    return listing_path.empty() ? entries[i].name : listing_path + "/" + entries[i].name;
}
std::string selected() {
    auto v = selected_indices(ctl(5));
    if (v.size() != 1)
        throw Error(Code::invalid, "此操作请选择一项");
    return remote_at(v[0]);
}
fs::path pick(bool save, const std::wstring& suggestion = L"") {
    wchar_t buffer[32768]{};
    wcsncpy_s(buffer, suggestion.c_str(), _TRUNCATE);
    OPENFILENAMEW o{};
    o.lStructSize = sizeof(o);
    o.hwndOwner = window;
    o.lpstrFile = buffer;
    o.nMaxFile = 32768;
    o.lpstrFilter = L"所有文件\0*.*\0";
    o.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | (save ? OFN_PATHMUSTEXIST : OFN_FILEMUSTEXIST);
    return (save ? GetSaveFileNameW(&o) : GetOpenFileNameW(&o)) ? fs::absolute(buffer) : fs::path{};
}
std::vector<fs::path> pick_many() {
    std::vector<wchar_t> buffer(32768);
    OPENFILENAMEW o{};
    o.lStructSize = sizeof(o);
    o.hwndOwner = window;
    o.lpstrFile = buffer.data();
    o.nMaxFile = DWORD(buffer.size());
    o.lpstrFilter = L"所有文件\0*.*\0";
    o.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT;
    if (!GetOpenFileNameW(&o)) {
        if (CommDlgExtendedError())
            throw Error(Code::invalid, "文件选择失败，所选路径可能过长");
        return {};
    }
    std::vector<fs::path> paths;
    auto cursor = buffer.data() + wcslen(buffer.data()) + 1;
    if (!*cursor)
        paths.push_back(fs::absolute(buffer.data()));
    else {
        fs::path base(buffer.data());
        while (*cursor) {
            if (paths.size() >= 32)
                throw Error(Code::invalid, "一次最多选择32个文件");
            paths.push_back(base / cursor);
            cursor += wcslen(cursor) + 1;
        }
    }
    return paths;
}
fs::path folder() {
    ComPtr<IFileDialog> d;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d))))
        throw Error(Code::unsupported, "目录选择器不可用");
    d->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (FAILED(d->Show(window)))
        return {};
    ComPtr<IShellItem> i;
    d->GetResult(&i);
    PWSTR p = nullptr;
    i->GetDisplayName(SIGDN_FILESYSPATH, &p);
    fs::path result(p);
    CoTaskMemFree(p);
    return result;
}
Picture jpeg(const Bytes& data) {
    return decode_jpeg(data);
}
void set_picture(Picture p) {
    std::lock_guard lock(updates_mutex);
    pending_picture = std::make_unique<Picture>(std::move(p));
    if (!picture_posted)
        picture_posted = PostMessageW(window, WM_IMAGE, 0, 0) != 0;
}
bool retry_wait(unsigned attempt, unsigned current, std::atomic_uint& generation_,
                std::atomic_bool& enabled) {
    for (unsigned t = 0;
         t < std::min(300u, 5u << std::min(attempt, 6u)) && !closing && enabled && current == generation_;
         ++t)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return !closing && enabled && current == generation_;
}
void video_loop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (!closing) {
        if (!subscribed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        unsigned e = video_epoch, retries = 0;
        auto c = creds();
        auto timeline = timeline_snapshot();
        bool fallback = false;
        while (!closing && subscribed && e == video_epoch) {
            try {
                auto client = c.connect(view, 1);
                video_connection.set(client);
                if (!subscribed || e != video_epoch)
                    break;
                if (fallback) {
                    gpu_picture = false;
                    status(L"JPEG兼容模式：CPU解码，最多30次请求/秒。");
                    while (!closing && subscribed && e == video_epoch) {
                        auto began = std::chrono::steady_clock::now();
                        Writer w;
                        w.u32(monitor_index);
                        w.u32(std::min(1920u, target_width.load()));
                        w.u32(70);
                        auto b = client->call(Op::frame, std::move(w));
                        Reader r{b};
                        auto encoded = r.blob();
                        r.end();
                        auto p = jpeg(encoded);
                        source_w = p.w;
                        source_h = p.h;
                        if (show_picture)
                            set_picture(std::move(p));
                        std::this_thread::sleep_until(began + std::chrono::milliseconds(33));
                    }
                } else {
                    auto capability = client->call(Op::capabilities);
                    Reader capability_reader{capability};
                    bool absolute = capability_reader.str().find("av-clock=host-qpc-v1") != std::string::npos;
                    capability_reader.end();
                    Writer w;
                    w.u32(absolute ? 2 : 1);
                    w.u32(monitor_index);
                    w.u32(target_width);
                    w.u32(target_fps);
                    w.u32(target_bitrate);
                    auto b = client->call(Op::desktop_stream, std::move(w));
                    Reader r{b};
                    if (r.u32() != (absolute ? 2u : 1u))
                        throw Error(Code::invalid, "Desktop version");
                    VideoConfig cfg;
                    cfg.width = r.u32();
                    cfg.height = r.u32();
                    cfg.fps = r.u32();
                    cfg.bitrate = r.u32();
                    cfg.capture_provider = r.str();
                    cfg.encoder_provider = r.str();
                    cfg.headers = r.blob(65536);
                    if (absolute && r.str() != "host-qpc-us")
                        throw Error(Code::invalid, "Desktop clock domain");
                    r.end();
                    WindowsVideo decoder(desktop, cfg, absolute ? timeline : nullptr,
                                         [e] { return !closing && subscribed && e == video_epoch; });
                    source_w = cfg.width;
                    source_h = cfg.height;
                    gpu_picture = true;
                    Writer ready;
                    ready.u32(1);
                    client->send(Op::desktop_ready, std::move(ready));
                    status(L"H.264 · GPU解码与渲染 · " + std::to_wstring(cfg.width) + L"×" +
                           std::to_wstring(cfg.height) + L" · " + std::to_wstring(cfg.fps) +
                           L" fps为请求上限");
                    bool first = true;
                    bool syncWarned = false;
                    while (!closing && subscribed && e == video_epoch) {
                        auto packet = client->receive(Op::desktop_packet);
                        Reader v{packet};
                        v.u64();
                        auto pts = v.u64();
                        v.u32();
                        auto payload = v.blob();
                        v.end();
                        if (first) {
                            payload.insert(payload.begin(), cfg.headers.begin(), cfg.headers.end());
                            first = false;
                        }
                        decoder.frame(payload, pts, show_picture && visible_desktop, zoom);
                        if (!syncWarned && timeline->fallbacks()) {
                            syncWarned = true;
                            status(L"音频设备延迟超过同步预算，当前优先及时显示画面，音画可能有偏差。");
                        }
                    }
                }
                break;
            } catch (const TransportError& error) {
                control_on = false;
                ++input_epoch;
                input_connection.interrupt();
                PostMessageW(window, WM_INPUT_FAILURE, 0, 0);
                if (!subscribed || e != video_epoch || closing)
                    break;
                if (retries >= 10) {
                    status(wide(error.what()));
                    subscribed = false;
                    break;
                }
                status(L"桌面断线，正在重连；控制已关闭。");
                retry_wait(retries++, e, video_epoch, subscribed);
            } catch (const Error& error) {
                if (!subscribed || e != video_epoch || closing)
                    break;
                if (error.code == Code::unsupported && !fallback) {
                    fallback = true;
                    status(L"硬件链路不可用，降级JPEG：" + wide(error.what()));
                } else {
                    status(L"桌面失败：" + wide(error.what()));
                    subscribed = false;
                    break;
                }
            } catch (const std::exception& error) {
                status(wide(error.what()));
                subscribed = false;
                break;
            }
        }
        video_connection.set({});
        gpu_picture = false;
    }
    CoUninitialize();
}
void audio_loop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (!closing) {
        if (!audio_on) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        auto c = creds();
        unsigned e = audio_epoch, retry = 0;
        auto timeline = timeline_snapshot();
        while (!closing && audio_on && e == audio_epoch) {
            try {
                audio_cancel.requested = false;
                auto client = c.connect(audio, 4);
                audio_connection.set(client);
                if (!audio_on || e != audio_epoch)
                    break;
                Writer w;
                w.u32(2);
                w.u32(WindowsAudio::supports_opus() ? 1 : 0);
                auto b = client->call(Op::audio_stream, std::move(w));
                Reader r{b};
                if (r.u32() != 2)
                    throw Error(Code::invalid, "Audio version");
                auto rate = r.u32(), channels = r.u32(), bits_ = r.u32();
                r.str();
                auto codec = r.u32(), lookahead = r.u32();
                r.end();
                if (channels != 2 || bits_ != 16)
                    throw Error(Code::unsupported, "Audio PCM16 stereo required");
                WindowsAudio renderer(rate, codec, lookahead, timeline);
                Writer ready;
                ready.u32(1);
                client->send(Op::audio_ready, std::move(ready));
                status(codec ? L"系统音频：Opus → WASAPI独立播放。" : L"系统音频：PCM → WASAPI独立播放。");
                while (!closing && audio_on && e == audio_epoch) {
                    auto packet = client->receive(Op::audio_packet);
                    Reader a{packet};
                    a.u64();
                    auto pts = a.u64();
                    auto flags = a.u32();
                    auto data = a.blob();
                    a.end();
                    renderer.packet(data, flags, audio_cancel, pts);
                }
                break;
            } catch (const TransportError& error) {
                if (!audio_on || e != audio_epoch || closing)
                    break;
                if (retry >= 10) {
                    status(wide(error.what()));
                    audio_on = false;
                    break;
                }
                status(L"音频断线，正在重建播放缓冲。");
                retry_wait(retry++, e, audio_epoch, audio_on);
            } catch (const std::exception& error) {
                if (audio_on && e == audio_epoch && !closing) {
                    status(L"音频失败：" + wide(error.what()));
                    audio_on = false;
                }
                break;
            }
        }
        audio_connection.set({});
    }
    CoUninitialize();
}
void input_loop() {
    std::shared_ptr<Client> client;
    unsigned current = 0;
    while (!closing) {
        InputEvent event;
        {
            std::unique_lock lock(input_mutex);
            input_cv.wait_for(lock, std::chrono::milliseconds(100),
                              [] { return closing || !input_queue.empty(); });
            if (closing)
                break;
            if (input_queue.empty()) {
                if (!control_on || current != input_epoch) {
                    client.reset();
                    input_connection.set({});
                }
                continue;
            }
            event = std::move(input_queue.front());
            input_queue.pop_front();
        }
        if (!control_on || event.epoch != input_epoch)
            continue;
        try {
            if (!client || current != input_epoch) {
                client = creds().connect(control, 0);
                current = event.epoch;
                input_connection.set(client);
            }
            if (!control_on || current != input_epoch)
                continue;
            client->call(event.op, std::move(event.body));
        } catch (const std::exception& error) {
            client.reset();
            input_connection.set({});
            control_on = false;
            ++input_epoch;
            {
                std::lock_guard lock(input_mutex);
                input_queue.clear();
            }
            PostMessageW(window, WM_INPUT_FAILURE, 0, 0);
            status(L"控制已关闭，未重放输入：" + wide(error.what()));
        }
    }
    input_connection.set({});
}
void add_column(HWND list, int index, const wchar_t* name, int width) {
    LVCOLUMNW c{};
    c.mask = LVCF_TEXT | LVCF_WIDTH;
    c.pszText = const_cast<wchar_t*>(name);
    c.cx = width;
    ListView_InsertColumn(list, index, &c);
}
void cell(HWND list, int row, int col, const std::wstring& value) {
    if (!col) {
        LVITEMW i{};
        i.mask = LVIF_TEXT;
        i.iItem = row;
        i.pszText = const_cast<wchar_t*>(value.c_str());
        ListView_InsertItem(list, &i);
    } else
        ListView_SetItemText(list, row, col, const_cast<wchar_t*>(value.c_str()));
}
constexpr uint64_t task_rates[] = {
    0, 65536, 1024 * 1024, 4 * 1024 * 1024, 8 * 1024 * 1024, 16 * 1024 * 1024, 32 * 1024 * 1024};
void remove_thumbnail_bitmap() {
    auto displayed = (HBITMAP)SendMessageW(ctl(66), STM_SETIMAGE, IMAGE_BITMAP, 0);
    if (displayed && displayed != thumbnail_bitmap)
        DeleteObject(displayed);
    if (thumbnail_bitmap)
        DeleteObject(thumbnail_bitmap);
    thumbnail_bitmap = nullptr;
}
void clear_preview() {
    ++thumbnail_ticket;
    last_preview_path.clear();
    if (thumbnails)
        thumbnails->cancel();
    remove_thumbnail_bitmap();
    SetWindowTextW(ctl(67), L"选择 JPG、PNG 或 BMP 图片查看预览。");
}
void preview_selection() {
    auto v = selected_indices(ctl(5));
    if (v.size() != 1 || size_t(v[0]) >= entries.size()) {
        clear_preview();
        return;
    }
    auto path = remote_at(v[0]);
    if (path == last_preview_path)
        return;
    clear_preview();
    last_preview_path = path;
    auto ext = fs::path(wide(path)).extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), towlower);
    if (entries[v[0]].directory || (ext != L".jpg" && ext != L".jpeg" && ext != L".png" && ext != L".bmp")) {
        SetWindowTextW(ctl(67), L"此类型暂无图片预览。");
        return;
    }
    SetWindowTextW(ctl(67), L"正在读取缩略图…");
    thumbnails->request(creds(), path, thumbnail_ticket);
}
void policy_selection() {
    if (replacing_tasks)
        return;
    auto v = selected_indices(ctl(45));
    if (v.size() != 1 || size_t(v[0]) >= task_snapshot.size())
        return;
    auto& t = task_snapshot[v[0]];
    if (t.id == policy_selected)
        return;
    policy_selected = t.id;
    SendMessageW(ctl(68), CB_SETCURSEL, t.priority, 0);
    size_t rate = 0;
    for (size_t i = 0; i < std::size(task_rates); ++i)
        if (task_rates[i] == t.rate_limit)
            rate = i;
    SendMessageW(ctl(69), CB_SETCURSEL, rate, 0);
}
void task_list() {
    auto indices = selected_indices(ctl(45));
    std::string old;
    if (!indices.empty() && size_t(indices[0]) < task_snapshot.size())
        old = task_snapshot[indices[0]].id;
    replacing_tasks = true;
    task_snapshot = transfers->snapshot();
    ListView_DeleteAllItems(ctl(45));
    const wchar_t* states[] = {L"排队", L"传输中", L"暂停", L"校验中", L"完成", L"失败/冲突", L"取消"};
    for (size_t i = 0; i < task_snapshot.size(); ++i) {
        auto& t = task_snapshot[i];
        cell(ctl(45), int(i), 0, states[int(t.state)]);
        cell(ctl(45), int(i), 1, (t.upload ? L"上传 " : L"下载 ") + wide(t.remote));
        cell(ctl(45), int(i), 2, bytes(t.current) + L" / " + bytes(t.total));
        cell(ctl(45), int(i), 3, wide(t.message));
        cell(ctl(45), int(i), 4, t.priority == 2 ? L"高" : t.priority == 0 ? L"低" : L"正常");
        cell(ctl(45), int(i), 5, t.rate_limit ? bytes(t.rate_limit) + L"/s" : L"自动");
        if (t.id == old)
            ListView_SetItemState(ctl(45), int(i), LVIS_SELECTED, LVIS_SELECTED);
    }
    replacing_tasks = false;
    policy_selection();
}
TransferTask selected_task() {
    auto v = selected_indices(ctl(45));
    if (v.size() != 1 || size_t(v[0]) >= task_snapshot.size())
        throw Error(Code::invalid, "请选择一个传输任务");
    return task_snapshot[v[0]];
}
void host_job(uint32_t kind, const std::string& source, const std::string& target) {
    auto c = creds();
    file_cancel.requested = true;
    file_connection.interrupt();
    enqueue([c, kind, source, target] {
        file_cancel.requested = false;
        auto client = c.connect(files_read | files_write, 0);
        file_connection.set(client);
        auto id = client_job(*client, kind, source, target);
        try {
            for (;;) {
                file_cancel.check();
                auto j = client_job_poll(*client, id);
                status(L"主机内任务 " + bytes(j.current) + L" / " + bytes(j.total));
                if (j.state == TaskState::completed) {
                    status(L"主机内操作完成，请刷新目录。");
                    break;
                }
                if (uint32_t(j.state) >= 5)
                    throw Error(j.error, j.message);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        } catch (...) {
            try {
                auto cc = c.connect(files_read | files_write, 0);
                client_job_cancel(*cc, id);
            } catch (...) {
            }
            throw;
        }
        file_connection.set({});
    });
}
std::string plain_name(const std::wstring& name) {
    if (name.find_first_of(L"/\\") != std::wstring::npos)
        throw Error(Code::invalid, "名称不能包含 / 或 \\；移动到其他文件夹请用“移动到”");
    return utf8(name);
}
void command(int id);
/** File actions that do not need a permanent button. at == nullptr anchors below the “更多” button. */
void file_menu(const POINT* at) {
    HMENU m = CreatePopupMenu();
    bool selection = !selected_indices(ctl(5)).empty();
    UINT needs = selection ? MF_STRING : MF_STRING | MF_GRAYED;
    if (at) {
        AppendMenuW(m, needs, 13, L"下载…");
        AppendMenuW(m, needs, 15, L"用默认应用打开");
        AppendMenuW(m, needs, 37, L"选择打开方式…");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(m, MF_STRING, 39, L"新建文件夹…");
    AppendMenuW(m, needs, 40, L"重命名…");
    AppendMenuW(m, needs, 41, L"移动到…");
    AppendMenuW(m, needs, 42, L"在电脑上复制…");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, needs, 44, L"文件信息与 SHA-256");
    AppendMenuW(m, needs, 52, L"原始媒体直读（外部播放器）");
    AppendMenuW(m, MF_STRING, 18, L"取消当前文件操作");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, needs, 43, L"删除…");
    POINT p{};
    if (at)
        p = *at;
    else {
        RECT r{};
        GetWindowRect(ctl(119), &r);
        p = {r.left, r.bottom + ui::dp(4)};
    }
    int picked = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, p.x, p.y, 0,
                                window, nullptr);
    DestroyMenu(m);
    if (picked > 0)
        command(picked);
}
void command(int id) {
    if (id >= 101 && id <= 104) {
        if (fullscreen)
            toggle_fullscreen();
        switch_page(id - 101);
        return;
    }
    if (id == 73) {
        status(L"正在发现局域网电脑…");
        enqueue([] {
            Cancel cancel;
            auto found = discover_devices(cancel);
            if (closing)
                return;
            Update u;
            u.discovery_result = true;
            u.discovered = std::move(found);
            publish(std::move(u));
        });
    } else if (id == 10) {
        if (clipboard_sync)
            clipboard_sync->stop();
        if (thumbnails)
            clear_preview();
        SetWindowTextW(ctl(65), L"开启文本剪贴板同步");
        SetWindowTextW(ctl(74), L"时序：响应优先");
        subscribed = false;
        audio_on = false;
        ++video_epoch;
        ++audio_epoch;
        video_connection.interrupt();
        audio_cancel.requested = true;
        audio_connection.interrupt();
        release_control();
        ClientCredentials c;
        auto endpoint = parse_endpoint(utf8(text(ctl(1))));
        c.host = endpoint.host;
        c.port = endpoint.port;
        c.pin = utf8(text(ctl(2)));
        c.token = utf8(text(ctl(3)));
        if (!valid_sha(c.pin) || !valid_sha(c.token))
            throw Error(Code::invalid, "指纹和密钥须为64位十六进制");
        connection_state = 4;
        enqueue([c] {
            std::string caps;
            try {
                auto client = c.connect(0, 0);
                auto b = client->call(Op::capabilities);
                Reader r{b};
                caps = r.str();
                r.end();
                save_credentials(state_dir / L"device.dpapi", c);
            } catch (...) {
                Update failed;
                failed.connection = 3;
                publish(std::move(failed));
                throw;
            }
            {
                std::lock_guard lock(credentials_mutex);
                credentials = c;
                av_timeline = std::make_shared<AvTimeline>();
            }
            Update u;
            u.connection = 2;
            u.status = L"设备已验证并保存（DPAPI）。" + wide(caps);
            publish(std::move(u));
        });
    } else if (id == 121) {
        // Pair with the one-time link copied from the host manager's scan-to-pair window.
        std::wstring prefill;
        if (OpenClipboard(window)) {
            if (auto data = GetClipboardData(CF_UNICODETEXT))
                if (auto p = static_cast<const wchar_t*>(GlobalLock(data))) {
                    std::wstring clip(p);
                    GlobalUnlock(data);
                    if (clip.starts_with(L"lantern://pair?"))
                        prefill = clip;
                }
            CloseClipboard();
        }
        auto entered = ui::prompt(window, L"用配对链接连接", L"粘贴电脑上“扫码添加设备”窗口里复制的链接：", prefill, L"下一步");
        if (!entered)
            return;
        auto link = parse_pairing_uri(utf8(*entered));
        if (!link)
            throw Error(Code::invalid, "链接无效或已过期，请在电脑上刷新二维码后重新复制");
        std::wstring groups;
        for (size_t i = 0; i < 16 && i + 4 <= link->pin.size(); i += 4)
            groups += wide(link->pin.substr(i, 4)) + L" ";
        auto question = L"将与这台电脑配对：\n\n名称：" + wide(link->host_name) + L"\n地址：" +
                        wide(format_endpoint({link->addresses.front(), link->port})) + L"\n指纹开头：" + groups +
                        L"…\n\n请确认它和电脑主机管理里显示的一致。";
        if (MessageBoxW(window, question.c_str(), L"确认电脑", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
            return;
        wchar_t name[256]{};
        DWORD size = 256;
        std::string label = "Windows 客户端";
        if (GetComputerNameExW(ComputerNamePhysicalDnsHostname, name, &size) && size)
            if (auto candidate = utf8(name) + " (Windows)"; valid_pairing_label(candidate))
                label = candidate;
        connection_state = 4;
        status(L"正在与 " + wide(link->host_name) + L" 配对…");
        enqueue([l = *link, label] {
            try {
                auto r = pair_with_link(l, label);
                ClientCredentials c;
                c.host = r.host;
                c.port = r.port;
                c.pin = r.pin;
                c.token = r.token;
                auto client = c.connect(0, 0);
                auto b = client->call(Op::capabilities);
                Reader rd{b};
                auto caps = rd.str();
                rd.end();
                save_credentials(state_dir / L"device.dpapi", c);
                {
                    std::lock_guard lock(credentials_mutex);
                    credentials = c;
                    av_timeline = std::make_shared<AvTimeline>();
                }
                Update u;
                u.connection = 2;
                u.paired = c;
                u.status = L"已配对并保存（DPAPI）。" + wide(caps);
                publish(std::move(u));
            } catch (const PairingError& e) {
                const wchar_t* why[] = {
                    L"链接无效或已过期，请在电脑上刷新二维码（也请检查两台电脑的时间）。",
                    L"电脑端版本过旧，请更新主机后重新生成二维码。",
                    L"配对码无效、已使用或已过期，请在电脑上刷新二维码。",
                    L"所有地址都连不上：请确认在同一局域网、主机已启动，并已允许通过防火墙。",
                    L"电脑身份与链接不一致，已停止配对。请重新核对电脑上的二维码。",
                    L"尝试过于频繁，请一分钟后再试。",
                    L"配对响应无效，没有保存任何凭据。请检查两端版本。",
                    L"连接中断，无法确认是否已配对。请在电脑的设备列表里检查，撤销多余条目后重新生成二维码。"};
                Update failed;
                failed.connection = 3;
                failed.status = why[size_t(e.reason)];
                publish(std::move(failed));
            } catch (...) {
                Update failed;
                failed.connection = 3;
                publish(std::move(failed));
                throw;
            }
        });
    } else if (id == 72) {
        wchar_t exe[32768]{};
        GetModuleFileNameW(nullptr, exe, 32768);
        auto manager = fs::path(exe).parent_path() / L"lantern-host.exe";
        if ((INT_PTR)ShellExecuteW(window, L"open", manager.c_str(), nullptr, manager.parent_path().c_str(),
                                   SW_SHOWNORMAL) <= 32)
            throw Error(Code::io, "主机管理程序不可用，请使用完整Windows包");
    } else if (id == 22) {
        auto c = creds();
        enqueue([c] {
            auto began = std::chrono::steady_clock::now();
            auto client = c.connect(0, 0);
            double tls = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began)
                             .count(),
                   sum = 0;
            for (int i = 0; i < 5; ++i) {
                Writer w;
                w.u64(i);
                auto t = std::chrono::steady_clock::now();
                auto b = client->call(Op::ping, std::move(w));
                Reader r{b};
                if (r.u64() != uint64_t(i))
                    throw Error(Code::invalid, "Ping nonce");
                r.end();
                sum +=
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
            }
            status(L"TLS建连 " + std::to_wstring(tls) + L" ms · RTT平均 " + std::to_wstring(sum / 5) +
                   L" ms（不是带宽测量）");
        });
    } else if (id == 11 || id == 33 || id == 36)
        refresh();
    else if (id == 19) {
        if (offset + entries.size() < total)
            refresh(offset + 256);
    } else if (id == 12) {
        auto p = utf8(text(ctl(4)));
        auto slash = p.rfind('/');
        SetWindowTextW(ctl(4), wide(slash == std::string::npos ? "" : p.substr(0, slash)).c_str());
        refresh();
    } else if (id == 34) {
        auto c = creds();
        auto p = utf8(text(ctl(4))), query = utf8(text(ctl(32)));
        unsigned e = ++listing_epoch;
        file_cancel.requested = true;
        file_connection.interrupt();
        enqueue([c, p, query, e] {
            file_cancel.requested = false;
            auto client = c.connect();
            file_connection.set(client);
            Update u;
            SearchPage pg;
            unsigned phase = 0;
            do {
                file_cancel.check();
                pg = client_search(*client, phase, p, query);
                phase = 1;
                u.page.entries.insert(u.page.entries.end(), pg.entries.begin(), pg.entries.end());
                if (u.page.entries.size() > 4096)
                    throw Error(Code::busy, "搜索结果达4096项，请缩小范围");
                status(L"正在搜索，已扫描 " + std::to_wstring(pg.scanned));
            } while (!pg.done);
            u.path = p;
            u.page.total = uint32_t(u.page.entries.size());
            u.listing = true;
            u.epoch = e;
            u.status = L"搜索完成";
            publish(std::move(u));
            file_connection.set({});
        });
    } else if (id == 18 || id == 35) {
        file_cancel.requested = true;
        file_connection.interrupt();
        status(L"已取消文件操作；传输任务请到传输页暂停。");
    } else if (id == 16) {
        subscribed = !subscribed;
        ++video_epoch;
        video_connection.interrupt();
        if (!subscribed)
            release_control();
        status(subscribed ? L"正在订阅H.264桌面…" : L"桌面采集/传输已停止，文件与音频独立。");
    } else if (id == 17) {
        show_picture = !show_picture;
        cancel_pointer();
        if (!show_picture)
            release_control();
        InvalidateRect(desktop, nullptr, TRUE);
        status(show_picture ? L"显示画面已开启" : L"仅隐藏本地画面；主机继续采集与传输。");
    } else if (id == 20) {
        if (control_on)
            release_control();
        else {
            if (!subscribed || !show_picture)
                throw Error(Code::invalid, "请先开启桌面画面");
            ++input_epoch;
            control_on = true;
        }
        status(control_on ? L"控制：拖拽、双击、右键、滚轮；Ctrl+滚轮缩放。" : L"控制已关闭");
    } else if (id == 23) {
        audio_on = !audio_on;
        ++audio_epoch;
        audio_cancel.requested = true;
        audio_connection.interrupt();
        status(audio_on ? L"正在订阅系统音频…" : L"音频订阅已停止。");
    } else if (id == 24)
        toggle_fullscreen();
    else if (id == 21) {
        release_control();
        auto c = creds();
        enqueue([c] {
            auto client = c.connect(view, 0);
            auto b = client->call(Op::monitors);
            Reader r{b};
            auto n = r.u32();
            if (!n || n > 64)
                throw Error(Code::unsupported, "没有显示器");
            for (uint32_t i = 0; i < n; ++i) {
                r.u32();
                r.u32();
                r.u32();
                r.u32();
                r.str();
            }
            r.end();
            monitor_index = (monitor_index + 1) % n;
            ++video_epoch;
            video_connection.interrupt();
            status(L"显示器 " + std::to_wstring(monitor_index + 1));
        });
    } else if (id == 25) {
        auto i = SendMessageW(ctl(25), CB_GETCURSEL, 0, 0);
        target_width = i == 0 ? 1280 : 1920;
        target_fps = i == 0 ? 30 : 60;
        target_bitrate = i == 0 ? 4'000'000 : i == 1 ? 8'000'000 : 12'000'000;
        ++video_epoch;
        video_connection.interrupt();
    } else if (id == 26) {
        zoom = 1;
        InvalidateRect(desktop, nullptr, TRUE);
    } else if (id == 28) {
        auto s = utf8(text(ctl(27)));
        if (s.empty())
            return;
        Writer w;
        w.str(s);
        input(Op::text_input, std::move(w));
        SetWindowTextW(ctl(27), L"");
    } else if (id == 29 || id == 30) {
        Writer w;
        w.u32(id == 29 ? VK_BACK : VK_RETURN);
        w.u32(1);
        input(Op::key_press, std::move(w));
    } else if (id == 56) {
        auto i = SendMessageW(ctl(56), CB_GETCURSEL, 0, 0);
        if (i >= 0 && i < 5) {
            const uint32_t mods[] = {1, 1, 1, 2, 8}, keys[] = {'C', 'V', 'A', VK_TAB, 'D'};
            Writer w;
            w.u32(mods[i]);
            w.u32(keys[i]);
            input(Op::shortcut, std::move(w));
        }
        SendMessageW(ctl(56), CB_SETCURSEL, WPARAM(-1), 0);
    } else if (id == 31) {
        if (fullscreen)
            toggle_fullscreen();
        switch_page(2);
    } else if (id == 39) {
        auto answer = ui::prompt(window, L"新建文件夹", L"在当前目录中新建文件夹，名称：", L"新建文件夹", L"新建");
        if (!answer || answer->empty())
            return;
        auto p = utf8(text(ctl(4))), name = plain_name(*answer);
        auto dest = p.empty() ? name : p + "/" + name;
        auto c = creds();
        enqueue([c, dest] {
            auto client = c.connect(files_read | files_write, 0);
            client->mutate(Op::mkdir, dest);
            status(L"目录已创建，请刷新。");
        });
    } else if (id == 40 || id == 41) {
        auto source = selected();
        auto slash = source.rfind('/');
        auto parent = slash == std::string::npos ? std::string{} : source.substr(0, slash + 1);
        std::string dest;
        if (id == 40) {
            auto answer = ui::prompt(window, L"重命名", L"新名称（保留在同一文件夹）：",
                                     wide(source.substr(parent.size())), L"重命名");
            if (!answer || answer->empty())
                return;
            dest = parent + plain_name(*answer);
        } else {
            auto answer = ui::prompt(window, L"移动到",
                                     L"目标路径，相对授权根目录并包含名称；在电脑上直接移动，不经过本机：",
                                     wide(source), L"移动");
            if (!answer || answer->empty())
                return;
            dest = utf8(*answer);
        }
        auto c = creds();
        enqueue([c, source, dest] {
            auto client = c.connect(files_read | files_write, 0);
            client->mutate(Op::rename, source, dest);
            status(L"主机内重命名/移动完成，请刷新。");
        });
    } else if (id == 42) {
        auto source = selected();
        auto answer = ui::prompt(window, L"在电脑上复制",
                                 L"副本路径，相对授权根目录并包含名称；在电脑上直接复制，不经过本机：",
                                 wide(source) + L" 副本", L"复制");
        if (!answer || answer->empty())
            return;
        host_job(0, source, utf8(*answer));
    } else if (id == 119)
        file_menu(nullptr);
    else if (id == 117 || id == 118) {
        chrome_hidden = id == 117;
        layout();
        if (!chrome_hidden)
            SetFocus(ctl(24));
    } else if (id == 43) {
        auto source = selected();
        if (MessageBoxW(window, (L"在电脑上永久删除“" + wide(source) + L"”？\n此操作不可撤销。").c_str(),
                        L"删除文件", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES)
            host_job(1, source, "");
    } else if (id == 44) {
        auto remote = selected();
        auto c = creds();
        enqueue([c, remote] {
            file_cancel.requested = false;
            auto client = c.connect();
            file_connection.set(client);
            auto info = client->stat(remote, true, &file_cancel);
            status(wide(remote) + L" · " + bytes(info.size) + L" · SHA256 " + wide(info.hash));
            file_connection.set({});
        });
    } else if (id == 14 || id == 55) {
        auto p = utf8(text(ctl(4)));
        auto c = creds();
        if (id == 55) {
            auto local = folder();
            if (local.empty())
                return;
            auto name = utf8(local.filename().wstring());
            transfers->add(c, p.empty() ? name : p + "/" + name, local, true, false, false, true);
        } else {
            auto files = pick_many();
            if (files.empty())
                return;
            std::vector<UploadSelection> items;
            for (auto& local : files) {
                auto name = utf8(local.filename().wstring());
                items.push_back({local, p.empty() ? name : p + "/" + name});
            }
            transfers->add_upload_batch(c, items);
        }
        switch_page(3);
    } else if (id == 13) {
        auto v = selected_indices(ctl(5));
        if (v.empty())
            throw Error(Code::invalid, "请选择文件");
        fs::path base;
        if (v.size() > 1 || entries[v[0]].directory) {
            base = folder();
            if (base.empty())
                return;
        }
        for (auto i : v) {
            auto remote = remote_at(i);
            auto local = v.size() == 1 && !entries[i].directory
                             ? pick(true, fs::path(wide(remote)).filename().wstring())
                             : base / fs::path(wide(remote)).filename();
            if (local.empty())
                return;
            transfers->add(creds(), remote, local, false, false, false, entries[i].directory);
        }
        switch_page(3);
    } else if (id == 15 || id == 37) {
        auto remote = selected();
        auto c = creds();
        auto ext = fs::path(wide(remote)).extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), towlower);
        if (ext != L".pdf" && ext != L".doc" && ext != L".docx" && ext != L".xls" && ext != L".xlsx" &&
            ext != L".ppt" && ext != L".pptx" && ext != L".txt" && ext != L".png" && ext != L".jpg" &&
            ext != L".mp4" && ext != L".mkv")
            throw Error(Code::unsupported, "此类型请下载后手动打开");
        enqueue([c, remote, id] {
            auto client = c.connect();
            auto info = client->stat(remote, false);
            if (info.size > 512ull * 1024 * 1024)
                throw Error(Code::unsupported, "打开缓存单文件限512MiB，大视频请下载后用播放器打开");
            auto cache = state_dir / L"cache";
            fs::create_directories(cache);
            uint64_t used = 0;
            for (auto& i : fs::recursive_directory_iterator(cache))
                if (i.is_regular_file())
                    used += i.file_size();
            if (used + info.size > 2ull * 1024 * 1024 * 1024)
                throw Error(Code::busy, "缓存达到2GiB，请在外部应用退出后清理");
            auto key = c.pin + remote + info.version;
            auto dest =
                cache / wide(sha256({(uint8_t*)key.data(), key.size()})) / fs::path(wide(remote)).filename();
            transfers->add(c, remote, dest, false, true, id == 37);
            status(L"已排队下载、校验并调用外部应用。");
        });
    } else if (id == 52) {
        auto remote = selected();
        auto c = creds();
        enqueue([c, remote] {
            auto next = std::make_unique<WindowsMediaBridge>(c, remote);
            auto url = next->url();
            {
                std::lock_guard lock(media_mutex);
                media_bridge = std::move(next);
                media_active = true;
            }
            Update u;
            u.status = L"原始媒体直读已开启：复制地址到播放器或点击“外部播放器”；字幕由接收应用处理。";
            u.path = url;
            u.epoch = UINT_MAX;
            publish(std::move(u));
        });
    } else if (id == 53) {
        auto url = utf8(text(ctl(62)));
        if (url.empty())
            throw Error(Code::invalid, "先选择文件并开启原始媒体直读");
        auto app = pick(false);
        if (app.empty())
            return;
        if (app.extension() != L".exe")
            throw Error(Code::invalid, "请选择已有播放器的exe");
        auto arg = L"\"" + wide(url) + L"\"";
        auto result = ShellExecuteW(window, L"open", app.c_str(), arg.c_str(), app.parent_path().c_str(),
                                    SW_SHOWNORMAL);
        if ((INT_PTR)result <= 32)
            throw Error(Code::unsupported, "外部播放器启动失败");
    } else if (id == 54) {
        SetWindowTextW(ctl(62), L"");
        enqueue([] {
            std::lock_guard lock(media_mutex);
            media_bridge.reset();
            media_active = false;
            status(L"原始媒体连接已停止，旧地址已失效。");
        });
    } else if (id == 74) {
        auto timeline = timeline_snapshot();
        bool sync = !timeline->synchronized();
        timeline->synchronize(sync);
        SetWindowTextW(ctl(74), sync ? L"时序：声音同步" : L"时序：响应优先");
        ++video_epoch;
        video_connection.interrupt();
        status(sync ? L"声音同步会增加画面等待，最多350ms；控制操作建议响应优先。"
                    : L"响应优先：画面不等待音频，声音继续独立播放。");
    } else if (id == 63 || id == 64) {
        clipboard_sync->copy_once(creds(), id == 63);
        SetWindowTextW(ctl(65), L"开启文本剪贴板同步");
        SetWindowTextW(ctl(74), L"时序：响应优先");
    } else if (id == 65) {
        if (clipboard_sync->enabled()) {
            clipboard_sync->stop();
            SetWindowTextW(ctl(65), L"开启文本剪贴板同步");
            SetWindowTextW(ctl(74), L"时序：响应优先");
            status(L"剪贴板同步已停止。");
        } else {
            clipboard_sync->start(creds());
            SetWindowTextW(ctl(65), L"停止文本剪贴板同步");
        }
    } else if (id == 70) {
        auto t = selected_task();
        auto priority = SendMessageW(ctl(68), CB_GETCURSEL, 0, 0),
             rate = SendMessageW(ctl(69), CB_GETCURSEL, 0, 0);
        if (priority < 0 || priority > 2 || rate < 0 || rate >= std::size(task_rates))
            throw Error(Code::invalid, "请选择优先级和限速");
        transfers->set_policy(t.id, uint32_t(priority), task_rates[rate]);
        status(L"已保存：限速作用于后续分块；优先级作用于下一个任务。");
    } else if (id == 46)
        transfers->pause(selected_task().id);
    else if (id == 47)
        transfers->resume(selected_task().id);
    else if (id == 48)
        transfers->remove(selected_task().id);
    else if (id == 49) {
        auto task = selected_task();
        auto remote = task.remote;
        auto local = task.local;
        if (task.upload) {
            auto answer = ui::prompt(window, L"另存到电脑",
                                     L"新的目标路径（相对授权根目录）；不会覆盖电脑上已有的文件：", wide(task.remote),
                                     L"另存");
            if (!answer || answer->empty())
                return;
            remote = utf8(*answer);
        } else {
            local = pick(true, task.local.filename().wstring());
            if (local.empty())
                return;
        }
        transfers->rename_destination(task.id, remote, local);
    } else if (id == 50) {
        auto task = selected_task();
        if (task.upload || task.state != TaskState::completed)
            throw Error(Code::invalid, "请选择完成的下载");
        open_external(task.local, true, window);
    }
}
LRESULT CALLBACK desktop_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    try {
        RECT r{};
        GetClientRect(h, &r);
        auto viewport = Viewport::fit(r.right, r.bottom, source_w, source_h, zoom);
        int x = short(LOWORD(lp)), y = short(HIWORD(lp));
        int32_t nx = 0, ny = 0;
        if (msg == WM_PAINT) {
            PAINTSTRUCT ps;
            auto dc = BeginPaint(h, &ps);
            if (!gpu_picture || !show_picture) {
                FillRect(dc, &r, ui::brush(ui::pal.chrome));
                if (!show_picture || picture.pixels.empty()) {
                    // Empty states: say which switch is responsible instead of showing a black box.
                    const wchar_t *title = !subscribed     ? ui_text::desktop_off
                                           : !show_picture ? ui_text::desktop_hidden
                                                           : ui_text::desktop_waiting,
                                  *hint = !subscribed     ? ui_text::desktop_off_hint
                                          : !show_picture ? ui_text::desktop_hidden_hint
                                                          : L"";
                    wchar_t g[2] = {show_picture ? ui::glyph::desktop : ui::glyph::hide, 0};
                    int cy = r.bottom / 2;
                    COLORREF soft = ui::mix(ui::pal.chrome_ink, ui::pal.chrome, .45);
                    ui::draw_text(dc, g, {0, cy - ui::dp(70), r.right, cy - ui::dp(24)}, ui::fonts.icon_large, soft,
                                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    ui::draw_text(dc, title, {0, cy - ui::dp(16), r.right, cy + ui::dp(10)}, ui::fonts.heading,
                                  ui::pal.chrome_ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    ui::draw_text(dc, hint, {ui::dp(16), cy + ui::dp(14), r.right - ui::dp(16), cy + ui::dp(38)},
                                  ui::fonts.note, soft, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                } else {
                    BITMAPINFO b{};
                    b.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                    b.bmiHeader.biWidth = picture.w;
                    b.bmiHeader.biHeight = -picture.h;
                    b.bmiHeader.biPlanes = 1;
                    b.bmiHeader.biBitCount = 32;
                    StretchDIBits(dc, viewport.left, viewport.top, viewport.width, viewport.height, 0, 0,
                                  picture.w, picture.h, picture.pixels.data(), &b, DIB_RGB_COLORS, SRCCOPY);
                }
            }
            EndPaint(h, &ps);
            return 0;
        }
        if (msg == WM_ERASEBKGND)
            return 1;
        if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) {
            SetFocus(h);
            if (control_on && show_picture && subscribed && viewport.map(x, y, nx, ny)) {
                dragging = true;
                pointer_x = nx;
                pointer_y = ny;
                SetCapture(h);
                pointer(1, nx, ny);
                SetTimer(h, 1, 350, nullptr);
            }
            return 0;
        }
        if (msg == WM_MOUSEMOVE && control_on && viewport.map(x, y, nx, ny, dragging)) {
            pointer_x = nx;
            pointer_y = ny;
            if (dragging)
                pointer(2, nx, ny);
            else {
                Writer w;
                w.u32(0);
                w.u32(nx);
                w.u32(ny);
                w.u32(0);
                w.u32(monitor_index);
                input(Op::input, std::move(w), true);
            }
            return 0;
        }
        if (msg == WM_LBUTTONUP && dragging) {
            viewport.map(x, y, nx, ny, true);
            dragging = false;
            pointer(3, nx, ny);
            KillTimer(h, 1);
            ReleaseCapture();
            return 0;
        }
        if (msg == WM_TIMER && dragging) {
            pointer(2, pointer_x, pointer_y);
            return 0;
        }
        if (msg == WM_CAPTURECHANGED || msg == WM_KILLFOCUS) {
            cancel_pointer();
            return 0;
        }
        if (msg == WM_RBUTTONUP && control_on && viewport.map(x, y, nx, ny)) {
            Writer w;
            w.u32(nx);
            w.u32(ny);
            w.u32(monitor_index);
            input(Op::right_click, std::move(w));
            return 0;
        }
        if (msg == WM_MOUSEWHEEL) {
            if (GetKeyState(VK_CONTROL) < 0) {
                zoom = std::clamp(zoom.load() + (short(HIWORD(wp)) > 0 ? 0.25 : -0.25), 1., 3.);
                InvalidateRect(h, nullptr, TRUE);
            } else {
                Writer w;
                w.u32(3);
                w.u32(uint32_t(int32_t(short(HIWORD(wp)))));
                w.u32(0);
                w.u32(0);
                w.u32(monitor_index);
                input(Op::input, std::move(w));
            }
            return 0;
        }
        uint32_t modifiers = (GetKeyState(VK_CONTROL) < 0 ? 1 : 0) | (GetKeyState(VK_MENU) < 0 ? 2 : 0) |
                             (GetKeyState(VK_SHIFT) < 0 ? 4 : 0) |
                             (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0 ? 8 : 0);
        if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wp != VK_CONTROL && wp != VK_SHIFT &&
            wp != VK_MENU && wp != VK_LWIN && wp != VK_RWIN &&
            ((modifiers & 11) || (wp >= VK_F1 && wp <= VK_F24) ||
             (modifiers && wp >= VK_PRIOR && wp <= VK_DOWN))) {
            Writer w;
            w.u32(modifiers);
            w.u32(uint32_t(wp));
            input(Op::shortcut, std::move(w));
            return 0;
        }
        if (msg == WM_KEYDOWN && (wp == VK_BACK || wp == VK_RETURN || wp == VK_TAB || wp == VK_ESCAPE ||
                                  wp == VK_DELETE || wp == VK_LEFT || wp == VK_RIGHT || wp == VK_UP ||
                                  wp == VK_DOWN || wp == VK_HOME || wp == VK_END)) {
            Writer w;
            w.u32(uint32_t(wp));
            w.u32(1);
            input(Op::key_press, std::move(w));
            return 0;
        }
        if (msg == WM_CHAR && wp >= 32 && wp != 127 && !(modifiers & 11)) {
            static wchar_t surrogate = 0;
            wchar_t ch = wchar_t(wp);
            std::wstring s;
            if (ch >= 0xd800 && ch <= 0xdbff) {
                surrogate = ch;
                return 0;
            }
            if (ch >= 0xdc00 && ch <= 0xdfff && surrogate) {
                s.push_back(surrogate);
                surrogate = 0;
            }
            s.push_back(ch);
            Writer w;
            w.str(utf8(s));
            input(Op::text_input, std::move(w));
            return 0;
        }
    } catch (const std::exception& e) {
        status(wide(e.what()));
    }
    return DefWindowProcW(h, msg, wp, lp);
}
LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (closing && msg == WM_COMMAND)
        return 0;
    try {
        switch (msg) {
        case WM_CREATE: {
            window = h;
            ui::dpi = GetDpiForWindow(h);
            ui::make_fonts();
            using ui::Kind;
            namespace g = ui::glyph;
            // Rail: navigation, host manager shortcut and the message line shared by every page.
            const wchar_t* tabs[] = {L"设备", L"桌面", L"文件", L"传输"};
            const wchar_t icons[] = {g::connect, g::desktop, g::folder, g::transfer};
            for (int i = 0; i < 4; ++i) {
                auto nav = button(101 + i, ALL_PAGES, Kind::Nav, tabs[i], icons[i], ui::Surface::Rail);
                ui::item(nav)->on = [i] { return page == i && !fullscreen; };
            }
            button(72, ALL_PAGES, Kind::Ghost, L"管理本机主机", g::settings, ui::Surface::Rail);
            statusbox = child(6, ALL_PAGES, L"STATIC", L"输入主机地址、指纹和此设备配对密钥。",
                              SS_CENTERIMAGE | SS_ENDELLIPSIS | SS_NOPREFIX);
            // Device.
            child(111, 0, L"STATIC", L"地址与端口", SS_CENTERIMAGE);
            child(1, 0, L"EDIT", L"127.0.0.1:47443", ES_AUTOHSCROLL | WS_TABSTOP);
            child(112, 0, L"STATIC", L"证书指纹", SS_CENTERIMAGE);
            child(2, 0, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP);
            child(113, 0, L"STATIC", L"配对密钥", SS_CENTERIMAGE);
            child(3, 0, L"EDIT", L"", ES_PASSWORD | ES_AUTOHSCROLL | WS_TABSTOP);
            SendMessageW(ctl(2), EM_SETCUEBANNER, TRUE, (LPARAM)L"64 位十六进制，与电脑主机管理中显示的一致");
            SendMessageW(ctl(3), EM_SETCUEBANNER, TRUE, (LPARAM)L"主机管理为此设备生成的密钥");
            button(10, 0, Kind::Primary, L"验证并保存", g::check, ui::Surface::Card);
            button(73, 0, Kind::Secondary, L"发现局域网电脑", g::search, ui::Surface::Card);
            button(121, 0, Kind::Secondary, L"用配对链接连接", g::link, ui::Surface::Card);
            button(22, 0, Kind::Secondary, L"网络诊断", g::diagnose, ui::Surface::Card);
            child(114, 0, L"STATIC", ui_text::privacy, SS_NOPREFIX);
            // Desktop: four independent switches, then picture tools, then keyboard and clipboard.
            tile(16, L"画面", g::desktop, L"已开启", L"未开启", [] { return subscribed.load(); });
            tile(17, L"隐藏本机画面", g::hide, L"已隐藏 · 仍在传输", L"显示中", [] { return !show_picture; });
            tile(20, L"控制", g::pointer, L"键鼠可操作", L"仅观看", [] { return control_on.load(); });
            tile(23, L"声音", g::speaker, L"已开启", L"关闭", [] { return audio_on.load(); });
            child(25, 1, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP);
            for (auto s : {L"流畅 720p30", L"均衡 1080p60", L"清晰 12Mbps"})
                SendMessageW(ctl(25), CB_ADDSTRING, 0, (LPARAM)s);
            SendMessageW(ctl(25), CB_SETCURSEL, 2, 0);
            button(21, 1, Kind::Secondary, L"切换显示器", g::monitors);
            button(74, 1, Kind::Secondary, L"时序：响应优先", g::clock);
            button(26, 1, Kind::Ghost, L"缩放复位", g::zoom);
            button(24, 1, Kind::Secondary, L"全屏", g::fullscreen);
            button(31, FULL_ONLY, Kind::Secondary, L"返回文件", g::folder);
            button(117, FULL_ONLY, Kind::Icon, L"收起工具栏", g::chevron_up);
            button(118, CHROME_HANDLE, Kind::Icon, L"显示工具栏", g::chevron_down, ui::Surface::Chrome);
            desktop = CreateWindowW(L"LanternDesktop", L"", WS_CHILD | WS_TABSTOP | WS_CLIPSIBLINGS, 0, 0, 1, 1, h,
                                    nullptr, GetModuleHandleW(nullptr), nullptr);
            child(27, 1, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP);
            SendMessageW(ctl(27), EM_SETCUEBANNER, TRUE, (LPARAM)L"输入文字，发送到电脑当前焦点");
            button(28, 1, Kind::Secondary, L"发送文字");
            button(29, 1, Kind::Ghost, L"退格");
            button(30, 1, Kind::Ghost, L"回车");
            child(56, 1, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP);
            for (auto s : {L"Ctrl+C 复制", L"Ctrl+V 粘贴", L"Ctrl+A 全选", L"Alt+Tab 切换", L"Win+D 桌面"})
                SendMessageW(ctl(56), CB_ADDSTRING, 0, (LPARAM)s);
            SendMessageW(ctl(56), CB_SETCUEBANNER, 0, (LPARAM)L"发送快捷键…");
            button(63, 1, Kind::Secondary, L"本机文本 → 电脑", g::paste);
            button(64, 1, Kind::Secondary, L"电脑文本 → 本机", g::copy);
            button(65, 1, Kind::Secondary, L"开启文本剪贴板同步", g::sync);
            // Files: location row, search row, actions, then list + preview.
            button(12, 2, Kind::Icon, L"上级文件夹", g::up);
            button(11, 2, Kind::Icon, L"刷新（F5）", g::refresh);
            child(4, 2, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP);
            SendMessageW(ctl(4), EM_SETCUEBANNER, TRUE, (LPARAM)L"授权根目录 · 输入相对路径后刷新");
            child(36, 2, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP);
            for (auto s : {L"名称 ↑", L"大小 ↑", L"修改时间 ↑", L"名称 ↓", L"大小 ↓", L"修改时间 ↓"})
                SendMessageW(ctl(36), CB_ADDSTRING, 0, (LPARAM)s);
            SendMessageW(ctl(36), CB_SETCURSEL, 0, 0);
            child(32, 2, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP);
            SendMessageW(ctl(32), EM_SETCUEBANNER, TRUE, (LPARAM)L"名称包含…");
            button(33, 2, Kind::Secondary, L"筛选本页", g::filter);
            button(34, 2, Kind::Secondary, L"搜索子文件夹", g::search);
            button(14, 2, Kind::Primary, L"上传文件", g::upload);
            button(55, 2, Kind::Secondary, L"上传文件夹", g::upload);
            button(13, 2, Kind::Secondary, L"下载", g::download);
            button(15, 2, Kind::Secondary, L"打开", g::open);
            button(37, 2, Kind::Ghost, L"打开方式…");
            button(119, 2, Kind::Ghost, L"更多", g::more);
            button(19, 2, Kind::Ghost, L"加载更多", g::chevron_down);
            auto list = child(5, 2, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | WS_TABSTOP);
            ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            add_column(list, 0, L"名称", 400);
            add_column(list, 1, L"类型", 80);
            add_column(list, 2, L"大小", 110);
            add_column(list, 3, L"修改时间", 170);
            child(66, 2, L"STATIC", L"", SS_BITMAP | SS_CENTERIMAGE);
            child(67, 2, L"STATIC", ui_text::preview_idle, SS_NOPREFIX);
            child(62, 2, L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL | WS_TABSTOP);
            button(53, 2, Kind::Secondary, L"外部播放器", g::video);
            button(54, 2, Kind::Danger, L"停止直读", g::stop);
            // Transfers: actions on the selected task, its policy, then the queue.
            button(46, 3, Kind::Secondary, L"暂停", g::pause);
            button(47, 3, Kind::Secondary, L"继续 / 重试", g::play);
            button(48, 3, Kind::Ghost, L"移除记录", g::remove);
            button(49, 3, Kind::Ghost, L"另存为…", g::save_as);
            button(50, 3, Kind::Ghost, L"打开下载的文件", g::open);
            child(71, 3, L"STATIC", L"优先级 / 速度上限", SS_CENTERIMAGE);
            child(68, 3, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP);
            child(69, 3, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP);
            for (auto label : {L"低（下个任务）", L"正常", L"高（下个任务）"})
                SendMessageW(ctl(68), CB_ADDSTRING, 0, (LPARAM)label);
            for (auto label :
                 {L"自动", L"64 KiB/s", L"1 MiB/s", L"4 MiB/s", L"8 MiB/s", L"16 MiB/s", L"32 MiB/s"})
                SendMessageW(ctl(69), CB_ADDSTRING, 0, (LPARAM)label);
            SendMessageW(ctl(68), CB_SETCURSEL, 1, 0);
            SendMessageW(ctl(69), CB_SETCURSEL, 0, 0);
            button(70, 3, Kind::Primary, L"应用", g::check);
            child(115, 3, L"STATIC", ui_text::tasks_note, SS_NOPREFIX);
            auto tasks = child(45, 3, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP);
            ListView_SetExtendedListViewStyle(tasks, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            add_column(tasks, 0, L"状态", 90);
            add_column(tasks, 1, L"文件", 340);
            add_column(tasks, 2, L"进度", 190);
            add_column(tasks, 3, L"结果 / 原因", 230);
            add_column(tasks, 4, L"优先级", 76);
            add_column(tasks, 5, L"速度上限", 96);
            for (int id : {12, 11, 117, 118})
                ui::tooltip(window, ctl(id), id == 12 ? L"上级文件夹" : id == 11 ? L"刷新（F5）"
                                                     : id == 117 ? L"收起工具栏" : L"显示工具栏");
            restyle();
            wchar_t* local = nullptr;
            if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
                throw Error(Code::io, "LocalAppData unavailable");
            state_dir = fs::path(local) / L"Lantern" / L"client";
            CoTaskMemFree(local);
            wchar_t override_dir[32768]{};
            if (GetEnvironmentVariableW(L"LANTERN_CLIENT_STATE", override_dir, 32768))
                state_dir = fs::absolute(override_dir);
            fs::create_directories(state_dir);
            if (fs::exists(state_dir / L"device.dpapi")) {
                credentials = load_credentials(state_dir / L"device.dpapi");
                SetWindowTextW(ctl(1), wide(format_endpoint({credentials.host, credentials.port})).c_str());
                SetWindowTextW(ctl(2), wide(credentials.pin).c_str());
                SetWindowTextW(ctl(3), wide(credentials.token).c_str());
                connection_state = 1;
            }
            transfers = std::make_unique<WindowsTransfers>(
                state_dir / L"transfers.dpapi", [] { PostMessageW(window, WM_TASKS, 0, 0); },
                [](const fs::path& p, bool chooser) {
                    Update u;
                    u.open = p;
                    u.chooser = chooser;
                    u.status = L"已校验，调用外部应用。";
                    publish(std::move(u));
                },
                [] { return (subscribed || audio_on ? 1u : 0u) | (media_active ? 2u : 0u); });
            clipboard_sync = std::make_unique<ClipboardSync>(window, [](std::wstring message) {
                if (!closing)
                    status(std::move(message));
            });
            thumbnails = std::make_unique<ThumbnailLoader>([](ThumbnailResult result) {
                if (closing)
                    return;
                std::lock_guard lock(updates_mutex);
                pending_thumbnail = std::make_unique<ThumbnailResult>(std::move(result));
                PostMessageW(window, WM_THUMB, 0, 0);
            });
            work_thread = std::thread([] {
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                for (;;) {
                    std::function<void()> f;
                    {
                        std::unique_lock lock(work_mutex);
                        work_cv.wait(lock, [] { return closing || !work.empty(); });
                        if (closing)
                            break;
                        f = std::move(work.front());
                        work.pop_front();
                    }
                    try {
                        f();
                    } catch (const std::exception& e) {
                        file_connection.set({});
                        status(wide(e.what()));
                    }
                }
                CoUninitialize();
            });
            video_thread = std::thread(video_loop);
            audio_thread = std::thread(audio_loop);
            input_thread = std::thread(input_loop);
            layout();
            task_list();
            return 0;
        }
        case WM_COMMAND: {
            auto id = LOWORD(wp), notice = HIWORD(wp);
            if (id == 68 || id == 69)
                return 0;
            if ((id == 25 || id == 36 || id == 56) && notice != CBN_SELCHANGE)
                return 0;
            if (id == 1 || id == 2 || id == 3 || id == 4 || id == 27 || id == 32 || id == 38 || id == 51 ||
                id == 62) {
                if (notice == EN_SETFOCUS || notice == EN_KILLFOCUS)
                    ui::invalidate_field(window, scene, (HWND)lp);
                return 0;
            }
            if (lp && notice != BN_CLICKED && notice != BN_DOUBLECLICKED && ui::item((HWND)lp))
                return 0;
            command(id);
            refresh_state();
            return 0;
        }
        case WM_PAINT:
            ui::paint(h, scene, paint_rail);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_DRAWITEM:
            ui::draw((DRAWITEMSTRUCT*)lp);
            return TRUE;
        case WM_DPICHANGED: {
            ui::dpi = HIWORD(wp);
            ui::make_fonts();
            apply_fonts();
            restyle();
            ui::window_icons(h);
            auto r = (RECT*)lp;
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            layout();
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (lp && wcscmp((const wchar_t*)lp, L"ImmersiveColorSet") == 0) {
                ui::load_palette();
                restyle();
                layout();
                RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
            }
            return 0;
        case WM_NOTIFY: {
            auto hdr = (NMHDR*)lp;
            if (hdr->code == LVN_GETEMPTYMARKUP && (hdr->idFrom == 5 || hdr->idFrom == 45)) {
                auto markup = (NMLVEMPTYMARKUP*)lp;
                markup->dwFlags = EMF_CENTERED;
                wcsncpy_s(markup->szMarkup,
                          hdr->idFrom == 45      ? ui_text::tasks_empty
                          : creds().host.empty() ? ui_text::files_unpaired
                                                 : ui_text::files_empty,
                          _TRUNCATE);
                return TRUE;
            }
            if (hdr->idFrom == 5 && hdr->code == NM_RCLICK) {
                POINT at{};
                GetCursorPos(&at);
                file_menu(&at);
                return 0;
            }
            if (hdr->idFrom == 5 && hdr->code == LVN_ITEMCHANGED)
                preview_selection();
            if (hdr->idFrom == 45 && hdr->code == LVN_ITEMCHANGED)
                policy_selection();
            if (hdr->idFrom == 5 && hdr->code == NM_DBLCLK) {
                auto v = selected_indices(ctl(5));
                if (v.size() == 1 && entries[v[0]].directory) {
                    SetWindowTextW(ctl(4), wide(remote_at(v[0])).c_str());
                    SetWindowTextW(ctl(32), L"");
                    refresh();
                }
            }
            return 0;
        }
        case WM_UPDATE: {
            std::deque<Update> list;
            {
                std::lock_guard lock(updates_mutex);
                list.swap(updates);
                update_posted = false;
            }
            if (clipboard_sync)
                SetWindowTextW(ctl(65),
                               clipboard_sync->enabled() ? L"停止文本剪贴板同步" : L"开启文本剪贴板同步");
            bool relayout = false;
            for (auto& u : list) {
                if (!u.status.empty())
                    SetWindowTextW(statusbox, u.status.c_str());
                if (u.connection >= 0)
                    connection_state = u.connection;
                if (u.paired) {
                    SetWindowTextW(ctl(1), wide(format_endpoint({u.paired->host, u.paired->port})).c_str());
                    SetWindowTextW(ctl(2), wide(u.paired->pin).c_str());
                    SetWindowTextW(ctl(3), wide(u.paired->token).c_str());
                }
                if (u.epoch == UINT_MAX && !u.listing) {
                    SetWindowTextW(ctl(62), wide(u.path).c_str());
                    relayout = true;
                }
                relayout = relayout || (u.listing && u.epoch == listing_epoch);
                if (u.listing && u.epoch == listing_epoch) {
                    clear_preview();
                    entries = std::move(u.page.entries);
                    listing_path = u.path;
                    generation = u.page.generation;
                    offset = u.offset;
                    total = u.page.total;
                    ListView_DeleteAllItems(ctl(5));
                    for (size_t i = 0; i < entries.size(); ++i) {
                        auto& e = entries[i];
                        cell(ctl(5), int(i), 0, wide(e.name));
                        cell(ctl(5), int(i), 1, e.directory ? L"目录" : L"文件");
                        cell(ctl(5), int(i), 2, e.directory ? L"—" : bytes(e.size));
                        FILETIME ft{DWORD(e.mtime), DWORD(e.mtime >> 32)}, local;
                        SYSTEMTIME st{};
                        FileTimeToLocalFileTime(&ft, &local);
                        FileTimeToSystemTime(&local, &st);
                        cell(ctl(5), int(i), 3,
                             std::to_wstring(st.wYear) + L"/" + std::to_wstring(st.wMonth) + L"/" +
                                 std::to_wstring(st.wDay) + L" " + std::to_wstring(st.wHour) + L":" +
                                 std::to_wstring(st.wMinute));
                    }
                }
                if (u.discovery_result) {
                    if (u.discovered.empty())
                        status(L"没有发现电脑。确认UDP47442可达，或手工输入地址。");
                    else {
                        HMENU menu = CreatePopupMenu();
                        for (size_t i = 0; i < u.discovered.size(); i++) {
                            auto label = wide(u.discovered[i].label + "  " +
                                              format_endpoint({u.discovered[i].host, u.discovered[i].port}));
                            std::replace(label.begin(), label.end(), L'&', L'_');
                            AppendMenuW(menu, MF_STRING, UINT_PTR(i + 1), label.c_str());
                        }
                        POINT at;
                        GetCursorPos(&at);
                        int picked = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, at.x, at.y, 0, window,
                                                    nullptr);
                        DestroyMenu(menu);
                        if (picked > 0 && size_t(picked) <= u.discovered.size()) {
                            auto& device = u.discovered[picked - 1];
                            SetWindowTextW(ctl(1), wide(format_endpoint({device.host, device.port})).c_str());
                            MessageBoxW(
                                window,
                                wide("发现声明的指纹：\n" + device.pin +
                                     "\n仅填写地址；指纹与密钥请在主机管理中核对后填写。发现结果不代表信任。")
                                    .c_str(),
                                L"尚未验证身份", MB_OK | MB_ICONINFORMATION);
                        }
                    }
                }
                if (!u.open.empty())
                    open_external(u.open, u.chooser, window);
            }
            if (relayout)
                layout();
            refresh_state();
            return 0;
        }
        case WM_THUMB: {
            std::unique_ptr<ThumbnailResult> result;
            {
                std::lock_guard lock(updates_mutex);
                result = std::move(pending_thumbnail);
            }
            if (!result || result->ticket != thumbnail_ticket)
                return 0;
            SetWindowTextW(ctl(67), result->message.c_str());
            auto& image = result->image;
            if (!image.pixels.empty()) {
                BITMAPINFO info{};
                info.bmiHeader = {sizeof(BITMAPINFOHEADER), image.w, -image.h, 1, 32, BI_RGB};
                void* pixels = nullptr;
                auto bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
                if (!bitmap)
                    win_error("Preview bitmap");
                memcpy(pixels, image.pixels.data(), image.pixels.size());
                remove_thumbnail_bitmap();
                SendMessageW(ctl(66), STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)bitmap);
                thumbnail_bitmap = bitmap;
            }
            return 0;
        }
        case WM_IMAGE: {
            std::lock_guard lock(updates_mutex);
            if (pending_picture) {
                picture = std::move(*pending_picture);
                pending_picture.reset();
            }
            picture_posted = false;
            InvalidateRect(desktop, nullptr, FALSE);
            return 0;
        }
        case WM_INPUT_FAILURE:
            cancel_pointer();
            refresh_state();
            return 0;
        case WM_TASKS:
            task_list();
            return 0;
        case WM_SIZE:
            if (!controls.empty())
                layout();
            return 0;
        case WM_GETMINMAXINFO:
            ((MINMAXINFO*)lp)->ptMinTrackSize = {ui::dp(980), ui::dp(660)};
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            auto c = (HWND)lp;
            int id = GetDlgCtrlID(c);
            COLORREF under = ui::under(scene, c);
            bool quiet = id == 6 || id == 67 || id == 71 || id == 114 || id == 115;
            SetTextColor((HDC)wp, quiet ? ui::pal.muted : ui::pal.ink);
            SetBkColor((HDC)wp, under);
            return (LRESULT)ui::brush(under);
        }
        case WM_CLOSE:
            if (closing)
                return 0;
            closing = true;
            subscribed = false;
            audio_on = false;
            file_cancel.requested = true;
            audio_cancel.requested = true;
            release_control();
            video_connection.interrupt();
            audio_connection.interrupt();
            file_connection.interrupt();
            work_cv.notify_all();
            input_cv.notify_all();
            SetWindowTextW(statusbox, L"正在结束连接并保存任务…");
            // Keep pumping: DXGI may marshal window operations to this UI thread.
            if (clipboard_sync)
                clipboard_sync->stop();
            if (thumbnails)
                thumbnails->cancel();
            shutdown_thread = std::thread([] {
                clipboard_sync.reset();
                thumbnails.reset();
                if (transfers)
                    transfers->stop();
                for (auto t : {&work_thread, &video_thread, &audio_thread, &input_thread})
                    if (t->joinable())
                        t->join();
                {
                    std::lock_guard lock(media_mutex);
                    media_bridge.reset();
                    media_active = false;
                }
                PostMessageW(window, WM_SHUTDOWN, 0, 0);
            });
            return 0;
        case WM_SHUTDOWN:
            if (shutdown_thread.joinable())
                shutdown_thread.join();
            remove_thumbnail_bitmap();
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
    } catch (const std::exception& e) {
        if (statusbox)
            SetWindowTextW(statusbox, wide(e.what()).c_str());
        else
            MessageBoxW(h, wide(e.what()).c_str(), L"Lantern", MB_OK | MB_ICONERROR);
    }
    return DefWindowProcW(h, msg, wp, lp);
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&common);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ui::startup();
    WNDCLASSEXW c{sizeof(c)};
    c.hInstance = instance;
    c.lpszClassName = L"LanternDesktop";
    c.lpfnWndProc = desktop_proc;
    c.style = CS_DBLCLKS;
    c.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&c);
    c.lpszClassName = L"LanternClient";
    c.lpfnWndProc = proc;
    c.hbrBackground = nullptr;
    ui::class_icons(c);
    RegisterClassExW(&c);
    auto h = CreateWindowW(c.lpszClassName, L"Lantern", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                           CW_USEDEFAULT, 1200, 800, nullptr, nullptr, instance, nullptr);
    if (!h)
        return 1;
    ui::window_icons(h);
    ui::initial_size(h, 1240, 820);
    ShowWindow(h, show);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        // Ctrl+1..4 switch pages and F5 refreshes files; never while keys go to the remote desktop.
        if (m.message == WM_KEYDOWN && m.hwnd != desktop && !fullscreen && !closing) {
            if (GetKeyState(VK_CONTROL) < 0 && m.wParam >= '1' && m.wParam <= '4') {
                PostMessageW(h, WM_COMMAND, 101 + (m.wParam - '1'), 0);
                continue;
            }
            if (m.wParam == VK_F5 && page == 2) {
                PostMessageW(h, WM_COMMAND, 11, 0);
                continue;
            }
        }
        if (m.message == WM_KEYDOWN && m.wParam == VK_F11) {
            toggle_fullscreen();
            continue;
        }
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE && fullscreen) {
            toggle_fullscreen();
            continue;
        }
        if (closing || m.hwnd == desktop || !IsDialogMessageW(h, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    transfers.reset();
    ui::shutdown();
    CoUninitialize();
    return 0;
}
