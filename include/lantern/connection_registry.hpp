#pragma once
#include "win.hpp"

namespace lantern {
// Host serializes registry, AuthenticationGuard and authenticated peers with the same mutex.
class ConnectionRegistry {
  public:
    using Clock = std::chrono::steady_clock;
    struct Entry {
        SOCKET socket;
        std::string address, source_bucket;
        Clock::time_point deadline;
        bool guarded, pending = true, timed_out = false, reserved = false;
    };
    using Ticket = std::shared_ptr<Entry>;
    explicit ConnectionRegistry(AuthenticationGuard& guard) : guard_(guard) {}
    Ticket admit(SOCKET socket, std::string address, bool guarded, Clock::time_point deadline, uint64_t now) {
        if (entries_.size() >= 64 || pending_ >= 32 || entries_.contains(socket))
            return {};
        auto ticket = std::make_shared<Entry>(Entry{socket, address, source_key(address), deadline, guarded});
        auto it = entries_.emplace(socket, ticket).first;
        try {
            if (guarded && !guard_.acquire(address, now)) {
                entries_.erase(it);
                return {};
            }
        } catch (...) {
            entries_.erase(it);
            throw;
        }
        ++pending_;
        return ticket;
    }
    bool promote(const Ticket& ticket) {
        if (!owns(ticket) || !ticket->pending || ticket->timed_out)
            return false;
        if (ticket->guarded)
            guard_.success(ticket->address);
        finish_handshake(*ticket);
        return true;
    }
    bool remove(const Ticket& ticket, bool timeout = false) noexcept {
        if (!owns(ticket))
            return false;
        if (ticket->pending) {
            finish_handshake(*ticket);
            if (ticket->guarded && !ticket->timed_out && (timeout || Clock::now() >= ticket->deadline)) {
                try { guard_.timeout(ticket->address); } catch (...) {} // allocation of diagnostic event
            }
        }
        entries_.erase(ticket->socket);
        return true;
    }
    const auto& entries() const { return entries_; }

  private:
    bool owns(const Ticket& ticket) const {
        auto it = entries_.find(ticket->socket);
        return it != entries_.end() && it->second == ticket;
    }
    void finish_handshake(Entry& ticket) noexcept {
        if (ticket.guarded)
            guard_.release_bucket(ticket.source_bucket);
        ticket.pending = false;
        --pending_;
    }
    AuthenticationGuard& guard_;
    std::map<SOCKET, Ticket> entries_;
    size_t pending_ = 0;
};
} // namespace lantern
