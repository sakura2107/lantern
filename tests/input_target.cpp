#include "lantern/win.hpp"
#include <fstream>
#include <windowsx.h>
using namespace lantern;
static std::ofstream log_file;
static int width, height;
static HWND editor, movable;
static bool interactions, held;
static LRESULT CALLBACK movable_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_EXITSIZEMOVE) {
        RECT rect{};
        GetWindowRect(window, &rect);
        log_file << "{\"kind\":\"window_move\",\"left\":" << rect.left << ",\"top\":" << rect.top << "}"
                 << std::endl;
    }
    if (message == WM_COMMAND && HIWORD(w) == EN_CHANGE && HWND(l) == editor) {
        wchar_t text[2048]{};
        GetWindowTextW(editor, text, 2048);
        // Only this generated fixture logs its test text. The product never records input text.
        std::string value, escaped;
        // WM_CHAR delivers UTF-16 pairs separately; skip the transient lone high surrogate.
        try {
            value = utf8(text);
        } catch (const Error&) {
            return 0;
        }
        for (char c : value) {
            if (c == '\r')
                escaped += "\\r";
            else if (c == '\n')
                escaped += "\\n";
            else if (c == '\\' || c == '\"') {
                escaped += '\\';
                escaped += c;
            } else
                escaped += c;
        }
        log_file << "{\"kind\":\"test_text\",\"value\":\"" << escaped << "\"}" << std::endl;
    }
    if (message == WM_CLOSE)
        return 0; // Keep owned canvas active until the harness finishes.
    return DefWindowProcW(window, message, w, l);
}
static LRESULT CALLBACK target_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        RECT bounds{};
        GetClientRect(window, &bounds);
        HBRUSH brush = CreateSolidBrush(RGB(24, 37, 54));
        FillRect(dc, &bounds, brush);
        DeleteObject(brush);
        SetTextColor(dc, RGB(240, 240, 245));
        SetBkMode(dc, TRANSPARENT);
        const wchar_t* title = L"Lantern input validation - generated canvas only - Esc aborts";
        TextOutW(dc, 30, 30, title, int(wcslen(title)));
        for (int y = 1; y <= 3; y++)
            for (int x = 1; x <= 3; x++) {
                int px = width * x / 4, py = height * y / 4;
                MoveToEx(dc, px - 22, py, nullptr);
                LineTo(dc, px + 22, py);
                MoveToEx(dc, px, py - 22, nullptr);
                LineTo(dc, px, py + 22);
                Ellipse(dc, px - 8, py - 8, px + 8, py + 8);
            }
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_LBUTTONUP) {
        POINT cursor{};
        GetCursorPos(&cursor);
        log_file << "{\"kind\":\"click\",\"x\":" << GET_X_LPARAM(l) << ",\"y\":" << GET_Y_LPARAM(l)
                 << ",\"cursor_x\":" << cursor.x << ",\"cursor_y\":" << cursor.y << "}" << std::endl;
        return 0;
    }
    if (message == WM_LBUTTONDOWN)
        SetFocus(window);
    if (message == WM_LBUTTONDBLCLK) {
        log_file << "{\"kind\":\"double_click\",\"x\":" << GET_X_LPARAM(l) << ",\"y\":" << GET_Y_LPARAM(l)
                 << "}" << std::endl;
        return 0;
    }
    if (message == WM_TIMER && w == 2) {
        bool now = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (now != held) {
            held = now;
            log_file << "{\"kind\":\"held\",\"down\":" << (held ? "true" : "false") << "}" << std::endl;
        }
        return 0;
    }
    if ((message == WM_KEYDOWN && w == VK_ESCAPE) || (message == WM_TIMER && w == 1) || message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2 && argc != 3)
        return 1;
    interactions = argc == 3;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    auto displays = monitors();
    if (displays.empty())
        return 2;
    auto monitor = displays[0];
    width = monitor.width;
    height = monitor.height;
    log_file.open(fs::path(argv[1]));
    if (!log_file)
        return 3;
    POINT old_cursor{};
    GetCursorPos(&old_cursor);
    HWND old_foreground = GetForegroundWindow();
    WNDCLASSW type{};
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpfnWndProc = target_proc;
    type.style = CS_DBLCLKS;
    type.lpszClassName = L"LanternOwnedInputTarget";
    type.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    RegisterClassW(&type);
    HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, type.lpszClassName,
                                  L"Lantern Input Validation", WS_POPUP, monitor.x, monitor.y, width, height,
                                  nullptr, nullptr, type.hInstance, nullptr);
    if (!window)
        return 4;
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    SetTimer(window, 1, 110000, nullptr);
    if (interactions) {
        WNDCLASSW secondary = type;
        secondary.lpfnWndProc = movable_proc;
        secondary.lpszClassName = L"LanternOwnedMovableTarget";
        RegisterClassW(&secondary);
        movable = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, secondary.lpszClassName,
                                  L"Drag this generated test window", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                  monitor.x + width / 5, monitor.y + height / 4, width * 2 / 5, height / 3,
                                  window, nullptr, type.hInstance, nullptr);
        editor = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL, 25, 70,
                                 width / 3, height / 7, movable, nullptr, type.hInstance, nullptr);
        ShowWindow(movable, SW_SHOW);
        SetForegroundWindow(movable);
        RECT frame{}, edit{};
        GetWindowRect(movable, &frame);
        GetWindowRect(editor, &edit);
        log_file << "{\"kind\":\"ready\",\"width\":" << width << ",\"height\":" << height
                 << ",\"left\":" << monitor.x << ",\"top\":" << monitor.y << ",\"window_left\":" << frame.left
                 << ",\"window_top\":" << frame.top << ",\"title_x\":" << (frame.left + width / 8 - monitor.x)
                 << ",\"title_y\":"
                 << (frame.top + GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CYCAPTION) / 2 -
                     monitor.y)
                 << ",\"edit_x\":" << ((edit.left + edit.right) / 2 - monitor.x)
                 << ",\"edit_y\":" << ((edit.top + edit.bottom) / 2 - monitor.y) << "}" << std::endl;
        SetTimer(window, 2, 50, nullptr);
    } else
        log_file << "{\"kind\":\"ready\",\"width\":" << width << ",\"height\":" << height
                 << ",\"left\":" << monitor.x << ",\"top\":" << monitor.y << "}" << std::endl;
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    SetCursorPos(old_cursor.x, old_cursor.y);
    if (IsWindow(old_foreground))
        SetForegroundWindow(old_foreground);
    return 0;
}
