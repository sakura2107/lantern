#include "lantern/windows_clipboard.hpp"
namespace lantern {
ClipboardText client_clipboard_read(Client& c) {
    Writer w;
    w.u32(1);
    auto b = c.call(Op::clipboard_read, std::move(w));
    Reader r{b};
    if (r.u32() != 1)
        throw Error(Code::invalid, "Clipboard version");
    ClipboardText t;
    t.sequence = r.u32();
    auto present = r.u32();
    if (present > 1)
        throw Error(Code::invalid, "Clipboard type");
    t.present = present != 0;
    t.text = r.str(65536);
    r.end();
    if (t.text.find('\0') != std::string::npos || (!t.present && !t.text.empty()))
        throw Error(Code::invalid, "Clipboard payload");
    wide(t.text);
    return t;
}
uint32_t client_clipboard_write(Client& c, uint32_t expected, const std::string& s) {
    if (s.size() > 65536 || s.find('\0') != std::string::npos)
        throw Error(Code::invalid, "Clipboard text bound");
    wide(s);
    Writer w;
    w.u32(1);
    w.u32(expected);
    w.str(s);
    auto b = c.call(Op::clipboard_write, std::move(w));
    Reader r{b};
    if (r.u32() != 1)
        throw Error(Code::invalid, "Clipboard version");
    auto sequence = r.u32();
    r.end();
    return sequence;
}
struct ClipboardSync::Impl {
    ClipboardAccess clipboard;
    std::function<void(std::wstring)> status;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv;
    bool closing = false, active = false;
    int once = 0;
    uint64_t generation = 0;
    ClientCredentials credentials;
    ActiveConnection connection;
    ~Impl() {
        {
            std::lock_guard lock(mutex);
            closing = true;
            active = false;
            ++generation;
            connection.interrupt();
        }
        cv.notify_all();
        if (thread.joinable())
            thread.join();
    }
    bool current(uint64_t e) {
        std::lock_guard lock(mutex);
        return !closing && generation == e;
    }
    void loop() {
        for (;;) {
            ClientCredentials c;
            uint64_t e;
            int command;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] { return closing || active || once; });
                if (closing)
                    return;
                c = credentials;
                e = generation;
                command = once;
                once = 0;
            }
            try {
                auto peer = c.connect(control, 0);
                connection.set(peer);
                if (!current(e))
                    continue;
                auto remote = client_clipboard_read(*peer);
                auto local = clipboard.read();
                if (!current(e))
                    continue;
                if (command) {
                    bool push = command == 1;
                    auto& source = push ? local : remote;
                    if (!source.present)
                        throw Error(Code::unsupported, "来源不是文本剪贴板，保留目的端内容");
                    if (push) {
                        if (clipboard.read().sequence != local.sequence)
                            throw Error(Code::conflict, "本地剪贴板已变化");
                        client_clipboard_write(*peer, remote.sequence, source.text);
                    } else {
                        auto now = client_clipboard_read(*peer);
                        if (now.sequence != remote.sequence)
                            throw Error(Code::conflict, "远程剪贴板已变化");
                        if (!current(e))
                            continue;
                        clipboard.write(local.sequence, source.text);
                    }
                    if (current(e))
                        status(push ? L"本地文本已复制到电脑剪贴板。" : L"电脑文本已复制到本地剪贴板。");
                } else {
                    status(L"文本剪贴板同步已开启；初始内容保持不变。");
                    for (;;) {
                        std::unique_lock lock(mutex);
                        cv.wait_for(lock, std::chrono::milliseconds(500),
                                    [&] { return closing || generation != e; });
                        if (closing || generation != e)
                            break;
                        lock.unlock();
                        auto nr = client_clipboard_read(*peer);
                        auto nl = clipboard.read();
                        bool lc = nl.sequence != local.sequence, rc = nr.sequence != remote.sequence;
                        if (lc && rc && nl.present && nr.present && nl.text != nr.text)
                            throw Error(Code::conflict,
                                        "两端剪贴板同时变化，同步已暂停；保留两端内容，可手动选择复制方向");
                        if (!current(e))
                            break;
                        if (lc && !rc && nl.present) {
                            if (clipboard.read().sequence != nl.sequence)
                                continue;
                            nr.sequence = client_clipboard_write(*peer, nr.sequence, nl.text);
                            nr.present = true;
                            nr.text = nl.text;
                        } else if (rc && !lc && nr.present) {
                            auto confirm = client_clipboard_read(*peer);
                            if (confirm.sequence != nr.sequence)
                                continue;
                            if (!current(e))
                                break;
                            nl.sequence = clipboard.write(nl.sequence, nr.text);
                            nl.present = true;
                            nl.text = nr.text;
                        }
                        local = std::move(nl);
                        remote = std::move(nr);
                    }
                }
            } catch (const std::exception& error) {
                bool report = false;
                {
                    std::lock_guard lock(mutex);
                    if (generation == e && !closing) {
                        active = false;
                        report = true;
                    }
                }
                if (report)
                    status(L"剪贴板操作停止：" + wide(error.what()));
            }
            connection.set({});
        }
    }
};
ClipboardSync::ClipboardSync(HWND owner, std::function<void(std::wstring)> status, ClipboardAccess access)
    : p_(std::make_unique<Impl>()) {
    if (!access.read && !access.write) {
        access.read = read_clipboard_text;
        access.write = [owner](uint32_t sequence, const std::string& text) {
            return write_clipboard_text(owner, sequence, text);
        };
    }
    if (!access.read || !access.write)
        throw Error(Code::invalid, "Clipboard provider requires read and write");
    p_->clipboard = std::move(access);
    p_->status = std::move(status);
    p_->thread = std::thread([this] { p_->loop(); });
}
ClipboardSync::~ClipboardSync() = default;
void ClipboardSync::start(ClientCredentials c) {
    std::lock_guard lock(p_->mutex);
    p_->credentials = std::move(c);
    ++p_->generation;
    p_->once = 0;
    p_->active = true;
    p_->connection.interrupt();
    p_->cv.notify_all();
}
void ClipboardSync::stop() {
    std::lock_guard lock(p_->mutex);
    ++p_->generation;
    p_->active = false;
    p_->once = 0;
    p_->connection.interrupt();
    p_->cv.notify_all();
}
void ClipboardSync::copy_once(ClientCredentials c, bool push) {
    std::lock_guard lock(p_->mutex);
    p_->credentials = std::move(c);
    ++p_->generation;
    p_->active = false;
    p_->once = push ? 1 : 2;
    p_->connection.interrupt();
    p_->cv.notify_all();
}
bool ClipboardSync::enabled() const {
    std::lock_guard lock(p_->mutex);
    return p_->active;
}
} // namespace lantern
