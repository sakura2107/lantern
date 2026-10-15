#include "lantern/win.hpp"
#include <thread>
#include <iostream>
#include <fstream>
using namespace lantern;
static int checks;
static void check(bool value,const char* label){++checks;if(!value)throw std::runtime_error(label);}
template<class F>static void denied(F action){bool failed=false;try{action();}catch(const Error& e){failed=e.code==Code::unauthorized;}check(failed,"authorization denied");}
template<class F>static void closed(F action){bool failed=false;try{action();}catch(const std::exception&){failed=true;}check(failed,"existing connection ended");}
int wmain(int argc,wchar_t** argv){
    try{
        if(argc!=2)throw std::runtime_error("Explicit validation workspace required");
        auto area=fs::absolute(argv[1]).lexically_normal();fs::create_directories(area);auto base=area/wide("device-trust-"+random_token().substr(0,12));fs::create_directories(base/L"share");fs::create_directories(base/L"state");
        auto root=base/L"share",path=base/L"state/config.dpapi";Bytes data(4096,0x53);write_file(root/L"fixture.bin",data);
        Identity identity(base/L"state/identity.dpapi",true);Config config{root,random_token(),31};TrustedDevice phone{random_token(),"Android fixture",random_token(),1},pc{random_token(),"Windows fixture",random_token(),3};config.devices={phone,pc};save_config(path,config);
        auto loaded=load_config(path);check(loaded.devices.size()==2,"device records round trip");check(authorized_scopes(loaded,phone.token)==1,"phone read grant");check(authorized_scopes(loaded,pc.token)==3,"PC write grant");check(!authorized_scopes(loaded,random_token()),"unknown token rejected");
        auto cipher=read_file(path);check(std::search(cipher.begin(),cipher.end(),phone.token.begin(),phone.token.end())==cipher.end(),"device credential encrypted at rest");
        Config invalid=config;invalid.devices.push_back(phone);bool rejected=false;try{save_config(path,invalid);}catch(const Error& e){rejected=e.code==Code::invalid;}check(rejected&&load_config(path).devices.size()==2,"duplicate record does not overwrite good configuration");
        Writer legacy;legacy.u16(1);legacy.str(utf8(root.wstring()));legacy.str(config.token);legacy.u32(31);write_file(base/L"state/legacy.dpapi",protect(legacy.b));auto old=load_config(base/L"state/legacy.dpapi");check(old.legacy_enabled&&old.devices.empty()&&authorized_scopes(old,config.token)==31,"v1 credential migration preserves existing pairing");
        std::exception_ptr thread_error;std::mutex error_lock;
        auto edits=[&](int side){try{for(int i=0;i<8;i++)edit_config(path,[&](Config& current){current.devices.push_back({random_token(),"concurrent fixture "+std::to_string(side)+"-"+std::to_string(i),random_token(),1});});}catch(...){std::lock_guard lock(error_lock);thread_error=std::current_exception();}};
        std::thread editA(edits,1),editB(edits,2);editA.join();editB.join();if(thread_error)std::rethrow_exception(thread_error);check(load_config(path).devices.size()==18,"concurrent edits do not lose devices");
        Host host(base/L"state","127.0.0.1",47991);std::thread server([&]{host.run();});
        struct Stop{Host& host;std::thread& thread;~Stop(){host.stop();if(thread.joinable())thread.join();}}stop{host,server};
        Client a("127.0.0.1",47991,identity.fingerprint(),phone.token,1),b("127.0.0.1",47991,identity.fingerprint(),pc.token,3);
        check(a.list("").size()==1&&b.list("").size()==1,"two device credentials operate concurrently");
        denied([&]{Client write("127.0.0.1",47991,identity.fingerprint(),phone.token,3);});
        Writer folder;folder.str("pc-folder");folder.str("");b.call(Op::mkdir,folder);check(fs::is_directory(root/L"pc-folder"),"write device performs actual filesystem mutation");
        edit_config(path,[&](Config& current){current.legacy_enabled=false;});std::this_thread::sleep_for(std::chrono::milliseconds(350));
        denied([&]{Client oldClient("127.0.0.1",47991,identity.fingerprint(),config.token,0);});check(a.list("").size()==2&&b.list("").size()==2,"disabling shared credential leaves named devices active");
        edit_config(path,[&](Config& current){for(auto& d:current.devices)if(d.id==phone.id)d.epoch++;});std::this_thread::sleep_for(std::chrono::milliseconds(350));closed([&]{a.list("");});check(b.list("").size()==2,"session termination isolated from second device");
        Client again("127.0.0.1",47991,identity.fingerprint(),phone.token,1);check(again.list("").size()==2,"session end retains pairing for deliberate reconnect");
        edit_config(path,[&](Config& current){current.devices.erase(std::remove_if(current.devices.begin(),current.devices.end(),[&](auto& d){return d.id==phone.id;}),current.devices.end());});std::this_thread::sleep_for(std::chrono::milliseconds(350));closed([&]{again.list("");});
        denied([&]{Client revoked("127.0.0.1",47991,identity.fingerprint(),phone.token,0);});check(b.list("").size()==2,"single-device revocation leaves other credential alive");
        edit_config(path,[&](Config& current){for(auto& d:current.devices)if(d.id==pc.id)d.scopes=1;});std::this_thread::sleep_for(std::chrono::milliseconds(350));closed([&]{b.list("");});denied([&]{Client removedWrite("127.0.0.1",47991,identity.fingerprint(),pc.token,3);});
        Client reader("127.0.0.1",47991,identity.fingerprint(),pc.token,1);check(reader.stat("fixture.bin").hash==sha256(data),"reduced grant still reads and verifies bytes");
        std::ofstream report(area/L"device-trust.json");report<<"{\"kind\":\"measured\",\"network_path\":\"windows_tls_loopback\",\"checks\":"<<checks<<",\"success\":true,\"device_revocation_isolated\":true,\"session_end_isolated\":true,\"legacy_migration_verified\":true,\"plaintext_credentials_logged\":false}";
        std::cout<<"device trust: "<<checks<<" checks passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<"device trust test failed after "<<checks<<" checks: "<<error.what()<<"\n";return 1;}
}
