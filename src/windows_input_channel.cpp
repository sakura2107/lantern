#include "lantern/windows_input_channel.hpp"
namespace lantern {
namespace {
struct WireInput final : InputTransport {
    std::shared_ptr<Client> client;
    std::string id;
    bool replay;
    WireInput(std::shared_ptr<Client> c,std::string key,bool safe):client(std::move(c)),id(std::move(key)),replay(safe){}
    bool replay_safe() const override {return replay;}
    void interrupt() noexcept override {client->interrupt();}
    InputResult exchange(uint64_t seq,Op op,const Bytes& bytes) override {
        Writer w;
        if(!replay) {w.b=bytes;try{auto bytes=client->call(op,std::move(w));return {Code::ok,"",std::move(bytes)};}
            catch(const TransportError&){throw;}catch(const Error& error){if(error.code==Code::unauthorized)throw;return {error.code,error.what(),{}};}}
        w.u32(1);w.str(id);w.u64(seq);w.u16(uint16_t(op));w.blob(bytes);
        auto response=client->call(Op::input_reliable,std::move(w));Reader r{response};
        if(r.u64()!=seq) throw Error(Code::conflict,"Input acknowledgement sequence");
        auto code=r.u32();auto message=r.str();auto payload=r.blob(8192);r.end();
        if(code==uint32_t(Code::unauthorized)) throw Error(Code::unauthorized,message);
        // Application failures are acknowledged too. Retrying could repeat a partially injected action.
        if(code>uint32_t(Code::source_changed))throw Error(Code::conflict,"Input result code");
        return {Code(code),std::move(message),std::move(payload)};
    }
};
}
std::unique_ptr<InputChannel> open_input_channel(ClientCredentials c,InputChannel::Events events,InputChannel::Results results) {
    struct State {bool automatic=false, flags=false;std::string id=random_token();};
    auto state=std::make_shared<State>();
    return std::make_unique<InputChannel>([c=std::move(c),state]() -> std::shared_ptr<InputTransport> {
        auto list=c.endpoints.empty()?std::vector<ConnectionEndpoint>{connection_endpoint(c.host,c.port)}:c.endpoints;
        auto connected=connect_endpoints(list,c.pin,std::chrono::seconds(3));
        auto client=std::make_shared<Client>(std::move(connected.tls),c.token,control,0,
            state->automatic&&state->flags?1:0);
        auto bytes=client->call(Op::capabilities); Reader r{bytes};auto caps=r.str();r.end();
        state->flags=caps.find(";session=end-v1")!=caps.npos;state->automatic=true;
        return std::make_shared<WireInput>(std::move(client),state->id,caps.find(";input-replay=v1")!=caps.npos);
    },std::move(events),std::move(results));
}
} // namespace lantern
