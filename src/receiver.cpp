#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include "receiver.hpp"
#include "truck_protocol.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>

namespace racing {
namespace {
using Clock=std::chrono::steady_clock;
using namespace std::chrono_literals;
bool uses_udp(Game game) { return game==Game::Forza || game==Game::FH6 || game==Game::F1; }
struct Mapping {
    HANDLE handle=nullptr;
    const uint8_t *view=nullptr;
    ~Mapping() { close(); }
    void close() { if(view) UnmapViewOfFile(view); if(handle) CloseHandle(handle); view=nullptr; handle=nullptr; }
    bool open(const wchar_t *name,size_t bytes) {
        if(view) return true;
        handle=OpenFileMappingW(FILE_MAP_READ,FALSE,name);
        if(!handle) return false;
        view=static_cast<const uint8_t*>(MapViewOfFile(handle,FILE_MAP_READ,0,0,bytes));
        if(!view) { close(); return false; }
        return true;
    }
};
}
struct Receiver::Impl {
    InputConfig config;
    mutable std::mutex mutex;
    Snapshot current;
    Clock::time_point received{};
    bool ever_received=false;
    std::atomic<bool> stop{false};
    std::condition_variable wake;
    std::mutex wait_mutex;
    std::thread worker;

    explicit Impl(InputConfig c):config(c) {
        current.status="Starting receiver";
        worker=std::thread([this] { run(); });
    }
    ~Impl() { stop=true; wake.notify_all(); if(worker.joinable()) worker.join(); }
    void wait(std::chrono::milliseconds duration) {
        std::unique_lock<std::mutex> lock(wait_mutex);
        wake.wait_for(lock,duration,[this] { return stop.load(); });
    }
    void status(const std::string &value,bool inactive=false) {
        std::lock_guard<std::mutex> lock(mutex);
        current.status=value; if(inactive) current.active=false;
    }
    void publish(const Telemetry &value,DecodeResult result,std::chrono::milliseconds age=0ms) {
        std::lock_guard<std::mutex> lock(mutex);
        if(result==DecodeResult::Inactive) {
            current.active=false; current.status="Game inactive"; ++current.packets;
        } else if(result==DecodeResult::Motion) {
            current.values=value; current.active=true; current.status="Receiving";
            received=Clock::now()-age; ever_received=true; ++current.packets;
        } else if(result==DecodeResult::Auxiliary) {
            current.values.speed=value.speed; current.values.rpm=value.rpm; current.values.gear=value.gear;
            ++current.packets; // Auxiliary packets never keep old motion alive.
        }
    }
    Snapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex);
        Snapshot result=current;
        result.age_seconds=ever_received?std::chrono::duration<double>(Clock::now()-received).count():1e9;
        result.active=result.active && result.age_seconds<0.5;
        if(!result.active && current.active && result.status=="Receiving") result.status="Telemetry stale";
        return result;
    }
    void run() {
        if(uses_udp(config.game)) udp();
        else if(config.game==Game::ETS2 || config.game==Game::ATS) trucks();
        else ac();
    }
    void udp() {
        WSADATA data{};
        if(WSAStartup(MAKEWORD(2,2),&data)!=0) { status("Winsock initialization failed",true); return; }
        SOCKET socket_handle=INVALID_SOCKET;
        Telemetry values{};
        while(!stop) {
            if(socket_handle==INVALID_SOCKET) {
                socket_handle=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
                if(socket_handle==INVALID_SOCKET) { status("Socket creation failed",true); wait(1000ms); continue; }
                BOOL exclusive=TRUE;
                if(setsockopt(socket_handle,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&exclusive),sizeof exclusive)!=0) {
                    status("Exclusive socket setup failed",true); closesocket(socket_handle); socket_handle=INVALID_SOCKET; wait(1000ms); continue;
                }
                sockaddr_in address{};
                address.sin_family=AF_INET;
                address.sin_addr.s_addr=htonl(config.lan?INADDR_ANY:INADDR_LOOPBACK);
                address.sin_port=htons(config.port);
                if(bind(socket_handle,reinterpret_cast<sockaddr*>(&address),sizeof address)!=0) {
                    status("Bind failed on UDP "+std::to_string(config.port)+" (error "+std::to_string(WSAGetLastError())+"); retrying",true);
                    closesocket(socket_handle); socket_handle=INVALID_SOCKET; wait(1000ms); continue;
                }
                u_long nonblocking=1;
                if(ioctlsocket(socket_handle,FIONBIO,&nonblocking)!=0) {
                    status("Nonblocking socket setup failed",true); closesocket(socket_handle); socket_handle=INVALID_SOCKET; wait(1000ms); continue;
                }
                status("Listening on "+std::string(config.lan?"0.0.0.0:":"127.0.0.1:")+std::to_string(config.port),true);
            }
            std::array<uint8_t,4096> buffer{};
            for(unsigned n=0;n<64 && !stop;++n) {
                const int bytes=recvfrom(socket_handle,reinterpret_cast<char*>(buffer.data()),static_cast<int>(buffer.size()),0,nullptr,nullptr);
                if(bytes==SOCKET_ERROR) {
                    const int error=WSAGetLastError();
                    if(error==WSAEWOULDBLOCK) break;
                    if(error==WSAEMSGSIZE || error==WSAECONNRESET) continue;
                    status("UDP receive failed; retrying",true); closesocket(socket_handle); socket_handle=INVALID_SOCKET; break;
                }
                const DecodeResult result=config.game==Game::F1
                    ?decode_f1(buffer.data(),static_cast<size_t>(bytes),values)
                    :decode_forza(buffer.data(),static_cast<size_t>(bytes),config.game==Game::FH6,values);
                publish(values,result);
            }
            wait(8ms);
        }
        if(socket_handle!=INVALID_SOCKET) closesocket(socket_handle);
        WSACleanup();
    }
    void ac() {
        Mapping physics,graphics;
        uint32_t last_id=0; bool seen=false;
        auto changed=Clock::now();
        while(!stop) {
            if(!physics.open(L"Local\\acpmf_physics",220) || !graphics.open(L"Local\\acpmf_graphics",8)) {
                physics.close(); graphics.close(); status("Waiting for AC/ACC shared memory",true); wait(250ms); continue;
            }
            std::array<uint8_t,220> p{}; std::array<uint8_t,8> g{};
            uint32_t before=0,after=0;
            std::memcpy(&before,physics.view,sizeof before); MemoryBarrier();
            std::memcpy(p.data(),physics.view,p.size()); std::memcpy(g.data(),graphics.view,g.size());
            MemoryBarrier(); std::memcpy(&after,physics.view,sizeof after);
            if(before==after) {
                Telemetry values{}; uint32_t id=0;
                DecodeResult result=decode_ac(p.data(),p.size(),g.data(),g.size(),values,id);
                if(result==DecodeResult::Inactive) {
                    // A graphics state change must take effect even with no new physics.
                    status("Game inactive",true);
                } else if(result==DecodeResult::Motion && (!seen || id!=last_id)) {
                    seen=true; last_id=id; changed=Clock::now(); publish(values,result);
                }
            }
            if(Clock::now()-changed>2s) {
                // Releasing stale maps allows a restarted game to recreate its mappings.
                // Keep last_id so reopening the same abandoned map cannot freshen it.
                physics.close(); graphics.close(); changed=Clock::now(); wait(250ms);
            } else wait(8ms);
        }
    }
    void trucks() {
        truck::Reader reader(config.game==Game::ATS);
        uint64_t last_sample=0; bool seen=false;
        auto changed=Clock::now();
        status("Waiting for SCS telemetry SDK plugin",true);
        while(!stop) {
            truck::Snapshot packet{};
            if(reader.read(packet)) {
                const uint64_t now=GetTickCount64();
                const bool valid=packet.magic==truck::kMagic && packet.version==truck::kVersion && packet.size==truck::kMappingSize;
                if(valid && !packet.active) status("Game inactive",true);
                else if(valid && packet.active && packet.timestamp_ms<=now && now-packet.timestamp_ms<500 && (!seen || packet.sample_counter!=last_sample)) {
                    Telemetry values{packet.surge/9.81f,packet.sway/9.81f,packet.heave/9.81f,packet.pitch,packet.roll,packet.yaw,packet.speed,packet.rpm,packet.gear};
                    if(finite(values)) {
                        seen=true; last_sample=packet.sample_counter; changed=Clock::now();
                        publish(values,DecodeResult::Motion,std::chrono::milliseconds(now-packet.timestamp_ms));
                    }
                }
            }
            if(Clock::now()-changed>2s) { reader.close(); changed=Clock::now(); }
            wait(8ms);
        }
    }
};
Receiver::Receiver(InputConfig config):impl_(new Impl(config)) {}
Receiver::~Receiver()=default;
Snapshot Receiver::snapshot() const { return impl_->snapshot(); }
std::shared_ptr<Receiver> Receiver::acquire(InputConfig config) {
    // Endpoint-independent transports ignore UDP settings.
    if(!uses_udp(config.game)) { config.port=0; config.lan=false; }
    using Key=std::tuple<Game,uint16_t,bool>;
    static std::mutex registry_mutex;
    static std::map<Key,std::weak_ptr<Receiver>> registry;
    std::lock_guard<std::mutex> lock(registry_mutex);
    for(auto it=registry.begin();it!=registry.end();) {
        if(it->second.expired()) it=registry.erase(it); else ++it;
    }
    Key key{config.game,config.port,config.lan};
    if(auto receiver=registry[key].lock()) return receiver;
    auto receiver=std::shared_ptr<Receiver>(new Receiver(config));
    registry[key]=receiver; return receiver;
}
}
