#include "lantern/pairing_client.hpp"
#include "lantern/endpoint_client.hpp"
namespace lantern {
PairResult pair_with_link(const PairingLink& link, const std::string& label) {
    PairingInvite invite;
    invite.host_name = link.host_name;
    invite.pin = link.pin;
    invite.code = link.code;
    invite.expires_at = link.expires_at;
    try {
        if (link.endpoints.empty())
            pairing_uri(invite, link.addresses, link.port);
        else
            pairing_uri_endpoints(invite, link.endpoints, link.port);
    } catch (...) {
        throw PairingError(PairError::invalid_link, Code::invalid, "配对链接无效或已过期");
    }
    if (!valid_pairing_label(label))
        throw PairingError(PairError::invalid_link, Code::invalid, "设备名称须为1..64字节");
    std::vector<ConnectionEndpoint> endpoints = link.endpoints;
    if (endpoints.empty())
        for (auto& address : link.addresses)
            endpoints.push_back(connection_endpoint(address, link.port));
    EndpointConnection connection;
    try {
        connection = connect_endpoints(endpoints, link.pin);
    } catch (const EndpointConnectError& e) {
        throw PairingError(
            e.reason == EndpointFailure::pin_mismatch ? PairError::pin_mismatch : PairError::unreachable,
            e.code,
            e.reason == EndpointFailure::pin_mismatch ? "主机证书指纹不匹配，请重新核对二维码"
                                                      : "所有地址均无法连接，请检查网络和防火墙");
    }
    auto& tls = connection.tls;
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
                throw PairingError(PairError::invalid_invite, code, "配对码无效或已过期，请在电脑刷新二维码");
            if (code == Code::busy)
                throw PairingError(PairError::busy, code, "配对尝试过于频繁或设备已满，请稍后重试");
            throw PairingError(PairError::protocol_error, code, "主机拒绝配对请求");
        }
        auto id = r.str(64), token = r.str(64);
        auto scopes = r.u32();
        r.end();
        if (!valid_sha(id) || !valid_sha(token) || !scopes || (scopes & ~31u))
            throw Error(Code::invalid, "Invalid pairing response");
        return {connection.selected.host, connection.selected.port, link.pin, id, token, scopes, endpoints};
    } catch (const PairingError&) {
        throw;
    } catch (...) {
        throw PairingError(PairError::outcome_unknown, Code::io,
                           "配对响应中断，结果无法确认；请检查电脑设备列表并重新生成邀请");
    }
}
} // namespace lantern
