#include "lantern/pairing.hpp"
#include <iostream>
using namespace lantern;
int main() {
    try {
        unsigned checks = 0;
        auto check = [&](bool value) {
            if (!value)
                throw std::runtime_error("Pairing link / QR assertion failed");
            ++checks;
        };
        PairingInvite invite;
        invite.host_name = "电脑 A&B";
        invite.pin = std::string(64, 'a');
        invite.code = std::string(64, 'b');
        invite.expires_at = pairing_unix_time() + 600;
        auto uri = pairing_uri(invite, {"192.168.1.5", "10.0.0.2"}, 47443);
        auto link = parse_pairing_uri(uri);
        check(link && link->host_name == invite.host_name && link->addresses.size() == 2 &&
              link->port == 47443 && link->code == invite.code);
        for (auto bad : {uri + "&v=1", uri + "&q=x", uri + "&", uri + "#x", std::string("https://pair?v=1"),
                         std::string(2049, 'a')})
            check(!parse_pairing_uri(bad));
        auto replace = [&](const std::string& from, const std::string& to) {
            auto bad = uri;
            auto at = bad.find(from);
            if (at == bad.npos)
                throw std::runtime_error("test fixture");
            bad.replace(at, from.size(), to);
            return bad;
        };
        for (auto version : {"v=0", "v=2", "v=01"})
            check(!parse_pairing_uri(replace("v=1", version)));
        for (auto port : {"p=0", "p=65536", "p=-1", "p=1x", "p=047443"})
            check(!parse_pairing_uri(replace("p=47443", port)));
        for (auto a : {"a=example.com", "a=127.1", "a=192.168.001.5", "a=256.1.1.1", "a=0.0.0.0",
                       "a=224.0.0.1", "a=192.168.1.5,192.168.1.5", "a=192.168.1.5,"})
            check(!parse_pairing_uri(replace("a=192.168.1.5,10.0.0.2", a)));
        for (auto name :
             {"n=%", "n=%GG", "n=%00", "n=%C0%AF", "n=%ED%A0%80", "n=%F4%90%80%80", "n=%0A", "n="})
            check(!parse_pairing_uri(replace("n=%E7%94%B5%E8%84%91%20A%26B", name)));
        check(!parse_pairing_uri(replace("x=" + std::to_string(invite.expires_at), "x=1")));
        check(!parse_pairing_uri(replace("f=" + invite.pin, "f=abcd")));
        check(!parse_pairing_uri(replace("c=" + invite.code, "c=wrong")));
        auto qr = qr_encode(uri);
        check(qr.size >= 21 && qr.size <= 177 && (qr.size - 21) % 4 == 0 &&
              qr.modules.size() == size_t(qr.size) * qr.size);
        auto small = qr_encode("hello");
        check(small.size == 21 && small.modules[0] && !small.modules[7 * 21]);
        auto large = qr_encode(std::string(2331, 'z'));
        check(large.size == 177);
        bool rejected = false;
        try {
            qr_encode(std::string(2332, 'z'));
        } catch (const Error& e) {
            rejected = e.code == Code::invalid;
        }
        check(rejected);
        rejected = false;
        try {
            qr_encode("");
        } catch (const Error&) {
            rejected = true;
        }
        check(rejected);
        std::cout << checks << " pairing-link/QR checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
