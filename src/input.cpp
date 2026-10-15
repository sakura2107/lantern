#include "lantern/win.hpp"
#include <cwctype>
namespace lantern {
static std::mutex input_mutex;
static InputSession* pointer_owner = nullptr;
static std::chrono::steady_clock::time_point pointer_deadline;
static void release_pointer() noexcept {
    INPUT up{};
    up.type = INPUT_MOUSE;
    up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    if (pointer_owner)
        SendInput(1, &up, sizeof(INPUT));
    pointer_owner = nullptr;
}
static uint32_t validation_pid = 0;
void set_input_validation_pid(uint32_t pid) {
    validation_pid = pid;
}
static void validate_input_target() {
    if (!validation_pid)
        return;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != validation_pid)
        throw Error(Code::cancelled, "Input validation canvas is no longer foreground");
}
static INPUT pointer_input(const Monitor& m, int32_t x, int32_t y) {
    if (x < 0 || x > 65535 || y < 0 || y > 65535)
        throw Error(Code::invalid, "Pointer coordinate");
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN),
        vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx =
        LONG((int64_t(m.x - vx) + int64_t(x) * (m.width - 1) / 65535) * 65535 / std::max(1, vw - 1));
    input.mi.dy =
        LONG((int64_t(m.y - vy) + int64_t(y) * (m.height - 1) / 65535) * 65535 / std::max(1, vh - 1));
    input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE;
    return input;
}
void remote_input(uint32_t kind, int32_t x, int32_t y, uint32_t value, uint32_t index) {
    std::lock_guard lock(input_mutex);
    if (pointer_owner)
        throw Error(Code::busy, "Another pointer gesture owns input");
    validate_input_target();
    auto v = monitors();
    if (index >= v.size())
        throw Error(Code::invalid, "Monitor index");
    INPUT i{};
    if (kind == 0) {
        i = pointer_input(v[index], x, y);
    } else if (kind == 1) {
        i.type = INPUT_MOUSE;
        switch (value) {
        case 0:
            i.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            break;
        case 1:
            i.mi.dwFlags = MOUSEEVENTF_LEFTUP;
            break;
        case 2:
            i.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
            break;
        case 3:
            i.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
            break;
        default:
            throw Error(Code::invalid, "Mouse button");
        }
    } else if (kind == 2) {
        if (value > 255 || x < 0 || x > 1)
            throw Error(Code::invalid, "Key input");
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = WORD(value);
        i.ki.dwFlags = x ? KEYEVENTF_KEYUP : 0;
    } else if (kind == 3) {
        if (x < -1200 || x > 1200)
            throw Error(Code::invalid, "Wheel bound");
        i.type = INPUT_MOUSE;
        i.mi.dwFlags = MOUSEEVENTF_WHEEL;
        i.mi.mouseData = DWORD(x);
    } else
        throw Error(Code::invalid, "Input kind");
    if (SendInput(1, &i, sizeof(i)) != 1)
        throw Error(Code::unsupported, "Input rejected (UIPI, secure desktop or session isolation)");
}

void expire_pointer_lease() noexcept {
    std::lock_guard lock(input_mutex);
    if (!pointer_owner)
        return;
    DWORD foreground_pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid);
    if (std::chrono::steady_clock::now() >= pointer_deadline ||
        (validation_pid && foreground_pid != validation_pid))
        release_pointer();
}
InputSession::~InputSession() {
    cancel();
}
void InputSession::cancel() noexcept {
    std::lock_guard lock(input_mutex);
    if (pointer_owner == this)
        release_pointer();
}
static void send_batch(std::span<INPUT> batch) {
    UINT sent = SendInput(UINT(batch.size()), batch.data(), sizeof(INPUT));
    if (sent != batch.size()) {
        // Compensate for partially injected presses. Never repeat the action itself.
        for (auto& i : batch) {
            if (i.type == INPUT_KEYBOARD && (i.ki.dwFlags & KEYEVENTF_KEYUP))
                SendInput(1, &i, sizeof(INPUT));
            else if (i.type == INPUT_MOUSE && (i.mi.dwFlags & (MOUSEEVENTF_LEFTUP | MOUSEEVENTF_RIGHTUP)))
                SendInput(1, &i, sizeof(INPUT));
        }
        throw Error(Code::unsupported, "Input rejected (UIPI or desktop isolation)");
    }
}
void InputSession::click(int32_t x, int32_t y, uint32_t index, bool twice) {
    std::lock_guard lock(input_mutex);
    if (pointer_owner)
        throw Error(Code::busy, "Pointer drag is active");
    validate_input_target();
    auto displays = monitors();
    if (index >= displays.size())
        throw Error(Code::invalid, "Monitor index");
    INPUT batch[5]{};
    batch[0] = pointer_input(displays[index], x, y);
    for (int i = 1; i < 5; ++i) {
        batch[i].type = INPUT_MOUSE;
        batch[i].mi.dwFlags = i % 2 ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    }
    send_batch(std::span<INPUT>(batch, twice ? 5 : 3));
}
void remote_click(int32_t x, int32_t y, uint32_t index) {
    InputSession session;
    session.click(x, y, index);
}
void InputSession::pointer(uint32_t phase, int32_t x, int32_t y, uint32_t index) {
    std::lock_guard lock(input_mutex);
    if (phase < 1 || phase > 4)
        throw Error(Code::invalid, "Pointer phase");
    if (phase == 4) {
        if (pointer_owner == this)
            release_pointer();
        return;
    }
    validate_input_target();
    auto displays = monitors();
    if (index >= displays.size())
        throw Error(Code::invalid, "Monitor index");
    INPUT batch[2]{};
    batch[0] = pointer_input(displays[index], x, y);
    batch[1].type = INPUT_MOUSE;
    if (phase == 1) {
        if (pointer_owner)
            throw Error(Code::busy, "Pointer lease already held");
        pointer_owner = this;
        batch[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    } else {
        if (pointer_owner != this)
            throw Error(Code::cancelled, "Pointer lease ended; start a new drag");
        batch[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    }
    try {
        send_batch(std::span<INPUT>(batch, phase == 2 ? 1 : 2));
        if (phase == 3)
            pointer_owner = nullptr;
        else
            pointer_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1800);
    } catch (...) {
        if (pointer_owner == this)
            release_pointer();
        throw;
    }
}
void InputSession::text(const std::string& text) {
    if (text.empty() || text.size() > 4096)
        throw Error(Code::invalid, "Text length bound");
    auto units = wide(text);
    if (units.size() > 1024)
        throw Error(Code::invalid, "Text UTF-16 bound");
    std::vector<INPUT> batch;
    batch.reserve(units.size() * 2);
    for (wchar_t unit : units) {
        if (unit < 32 || unit == 127)
            throw Error(Code::invalid, "Control characters require key operation");
        INPUT down{};
        down.type = INPUT_KEYBOARD;
        down.ki.wScan = unit;
        down.ki.dwFlags = KEYEVENTF_UNICODE;
        batch.push_back(down);
        down.ki.dwFlags |= KEYEVENTF_KEYUP;
        batch.push_back(down);
    }
    std::lock_guard lock(input_mutex);
    if (pointer_owner)
        throw Error(Code::busy, "Finish drag before text input");
    validate_input_target();
    send_batch(batch);
}
void InputSession::key(uint32_t vk, uint32_t count) {
    bool allowed = vk == VK_BACK || vk == VK_RETURN || vk == VK_TAB || vk == VK_ESCAPE || vk == VK_DELETE ||
                   vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_HOME ||
                   vk == VK_END;
    if (!allowed || count < 1 || count > 32)
        throw Error(Code::invalid, "Atomic key bound");
    std::vector<INPUT> batch(count * 2);
    for (uint32_t i = 0; i < count; ++i) {
        batch[i * 2].type = batch[i * 2 + 1].type = INPUT_KEYBOARD;
        batch[i * 2].ki.wVk = batch[i * 2 + 1].ki.wVk = WORD(vk);
        batch[i * 2 + 1].ki.dwFlags = KEYEVENTF_KEYUP;
    }
    std::lock_guard lock(input_mutex);
    if (pointer_owner)
        throw Error(Code::busy, "Finish drag before key input");
    validate_input_target();
    send_batch(batch);
}
uint32_t InputSession::focus() {
    validate_input_target();
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    DWORD thread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    if (!thread || !GetGUIThreadInfo(thread, &info) || !info.hwndFocus)
        return 0;
    wchar_t name[128]{};
    GetClassNameW(info.hwndFocus, name, 128);
    std::wstring type(name);
    std::transform(type.begin(), type.end(), type.begin(), towlower);
    bool known = type == L"edit" || type.rfind(L"richedit", 0) == 0;
    LONG_PTR style = GetWindowLongPtrW(info.hwndFocus, GWL_STYLE);
    if (!known || !IsWindowEnabled(info.hwndFocus) || (style & ES_READONLY))
        return 0;
    return 1 | ((style & ES_PASSWORD) ? 2 : 0);
}
void InputSession::shortcut(uint32_t modifiers, uint32_t vk) {
    if (modifiers > 15 || !vk || vk > 255 || vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT ||
        vk == VK_LWIN || vk == VK_RWIN)
        throw Error(Code::invalid, "Atomic shortcut bound");
    if ((modifiers & 3) == 3 && vk == VK_DELETE)
        throw Error(Code::unsupported, "Secure attention sequence requires a privileged system service");
    std::vector<INPUT> batch;
    auto key = [&](WORD key, bool up) {
        INPUT i{};
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = key;
        i.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
        batch.push_back(i);
    };
    const WORD mods[] = {VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN};
    for (unsigned i = 0; i < 4; ++i)
        if (modifiers & (1u << i))
            key(mods[i], false);
    key(WORD(vk), false);
    key(WORD(vk), true);
    for (int i = 3; i >= 0; --i)
        if (modifiers & (1u << i))
            key(mods[i], true);
    std::lock_guard lock(input_mutex);
    if (pointer_owner)
        throw Error(Code::busy, "Finish drag before shortcut");
    validate_input_target();
    send_batch(batch);
}
void InputSession::right_click(int32_t x, int32_t y, uint32_t index) {
    std::lock_guard lock(input_mutex);
    if (pointer_owner)
        throw Error(Code::busy, "Finish drag before right click");
    validate_input_target();
    auto displays = monitors();
    if (index >= displays.size())
        throw Error(Code::invalid, "Monitor index");
    INPUT batch[3]{};
    batch[0] = pointer_input(displays[index], x, y);
    batch[1].type = batch[2].type = INPUT_MOUSE;
    batch[1].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
    batch[2].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
    send_batch(batch);
}
} // namespace lantern
