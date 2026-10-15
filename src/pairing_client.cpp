#include "lantern/pairing_client.hpp"
namespace lantern {
PairResult pair_with_link(const PairingLink& link, const std::string& label) {
    PairingInvite invite;
    invite.host_name = link.host_name;
    invite.pin = link.pin;
    invite.code = link.code;
    invite.expires_at = link.expires_at;
    try {
        pairing_uri(invite, link.addresses, link.port);
    } catch (...) {
        throw PairingError(PairError::invalid_link, Code::invalid, "配对链接无效或已过期");
    }
    if (!valid_pairing_label(label))
        throw PairingError(PairError::invalid_link, Code::invalid, "设备名称须为1..64字节");
    bool pin_failed = false;
    for (auto& address : link.addresses) {
        std::unique_ptr<Tls> tls;
        try {
            tls = std::make_unique<Tls>(connect_socket(address, link.port), nullptr, link.pin);
        } catch (const Error& e) {
            if (e.code == Code::unauthorized)
                pin_failed = true;
            continue;
        }
        try {
            Writer w;
            w.u16(protocol_version);
            w.u16(uint16_t(Op::pair));
            w.str(link.code);
            w.str(label);
            tls->send(w.b);
            auto bytes = tls->receive();
            Reader r{bytes};
            auto version = r.u16(), op = r.u16();
            auto code = Code(r.u32());
            if (code == Code::unsupported && (op == uint16_t(Op::pair) || op == uint16_t(Op::hello)))
                throw PairingError(PairError::host_too_old, code, "主机版本过旧，请更新电脑端");
            if (version != protocol_version || op != uint16_t(Op::pair))
                throw PairingError(PairError::protocol_error, Code::invalid, "主机配对响应无效");
            if (code != Code::ok) {
                r.str(1024);
                r.end(); // Never surface remote text: it may contain credentials.
                if (code == Code::unauthorized)
                    throw PairingError(PairError::invalid_invite, code,
                                       "配对码无效或已过期，请在电脑刷新二维码");
                if (code == Code::busy)
                    throw PairingError(PairError::busy, code, "配对尝试过于频繁或设备已满，请稍后重试");
                throw PairingError(PairError::protocol_error, code, "主机拒绝配对请求");
            }
            auto id = r.str(64), token = r.str(64);
            auto scopes = r.u32();
            r.end();
            if (!valid_sha(id) || !valid_sha(token) || !scopes || (scopes & ~31u))
                throw Error(Code::invalid, "Invalid pairing response");
            return {address, link.port, link.pin, id, token, scopes};
        } catch (const PairingError&) {
            throw;
        } catch (...) {
            throw PairingError(PairError::outcome_unknown, Code::io,
                               "配对响应中断，结果无法确认；请检查电脑设备列表并重新生成邀请");
        }
    }
    if (pin_failed)
        throw PairingError(PairError::pin_mismatch, Code::unauthorized,
                           "主机证书指纹不匹配，请重新核对二维码");
    throw PairingError(PairError::unreachable, Code::io, "所有地址均无法连接，请检查同一局域网和防火墙");
}
} // namespace lantern
