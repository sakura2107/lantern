#pragma once
#include "core.hpp"
#include <array>
#include <bitset>
#include <functional>
#include <map>
#include <memory>
#include <optional>
namespace lantern {
constexpr uint32_t datagram_payload=1024;
enum class DatagramType:uint16_t { data=1,parity=2,nack=3,hello=4,challenge=5,proof=6,feedback=7 };
struct Datagram {
    DatagramType type;uint64_t sequence=0,frame=0;
    uint32_t index=0,count=0,total=0;
    Bytes payload;
};
// Single owning sender/receiver. Separate directional keys and prefixes; TLS exchanges fresh per-session keys.
class DatagramCipher {
    struct Impl;std::unique_ptr<Impl> impl_;
  public:
    DatagramCipher(std::span<const uint8_t> key,std::span<const uint8_t> session,uint32_t direction);
    ~DatagramCipher();
    Bytes seal(Datagram);
    std::optional<Datagram> open(std::span<const uint8_t>); // Bad tag, header, replay => silent discard.
};
// One frame => fragments + one XOR parity fragment per group of eight. No compression of encoded media.
void fragment_datagram(uint64_t frame,std::span<const uint8_t>,const std::function<void(Datagram)>&);
class DatagramFrames {
    struct Frame {uint32_t total,count;uint64_t began,last_nack=0;std::map<uint32_t,Bytes> data,parity;};
    std::map<uint64_t,Frame> frames_;
    uint64_t delivered_=0;bool have_delivered_=false;
  public:
    std::optional<Bytes> accept(const Datagram&,uint64_t now_ms);
    std::vector<uint32_t> missing(uint64_t frame,uint64_t now_ms,uint32_t retention_ms=160) const;
    std::optional<std::pair<uint64_t,std::vector<uint32_t>>> nack(uint64_t now_ms,uint32_t retention_ms=160);
    void clear(){frames_.clear();}
};
} // namespace lantern
