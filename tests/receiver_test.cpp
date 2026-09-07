#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include "receiver.hpp"
#include "truck_protocol.hpp"
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>
using namespace racing;
using namespace std::chrono;
static void require(bool v,const char *why) { if(!v) { std::cerr<<"FAIL: "<<why<<'\n'; std::exit(1); } }
template<class T> static void put(std::vector<uint8_t>&b,size_t p,T v) { std::memcpy(b.data()+p,&v,sizeof v); }
template<class F> static bool until(F f,int milliseconds=1500) {
    auto end=steady_clock::now()+std::chrono::milliseconds(milliseconds);
    while(steady_clock::now()<end) { if(f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    return f();
}
static uint16_t free_port() {
    SOCKET s=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP); require(s!=INVALID_SOCKET,"port probe socket");
    sockaddr_in a{}; a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    require(bind(s,reinterpret_cast<sockaddr*>(&a),sizeof a)==0,"port probe bind");
    int n=sizeof a; getsockname(s,reinterpret_cast<sockaddr*>(&a),&n); closesocket(s); return ntohs(a.sin_port);
}
static void send_packet(SOCKET s,uint16_t port,const std::vector<uint8_t>&p) {
    sockaddr_in a{}; a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_LOOPBACK); a.sin_port=htons(port);
    require(sendto(s,reinterpret_cast<const char*>(p.data()),static_cast<int>(p.size()),0,reinterpret_cast<sockaddr*>(&a),sizeof a)==static_cast<int>(p.size()),"send fixture");
}
int main() {
    WSADATA w{}; require(WSAStartup(MAKEWORD(2,2),&w)==0,"Winsock initialization");
    const uint16_t port=free_port(); InputConfig config{Game::Forza,port,false};
    auto r=Receiver::acquire(config), same=Receiver::acquire(config);
    require(r==same,"identical endpoint shares receiver");
    require(until([&]{return r->snapshot().status.find("Listening")!=std::string::npos;}),"UDP listener ready");
    auto conflict=Receiver::acquire({Game::F1,port,false});
    require(until([&]{return conflict->snapshot().status.find("Bind failed")!=std::string::npos;}),"exclusive port rejects different game");
    SOCKET sender=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    std::vector<uint8_t> p(232); put<int32_t>(p,0,1); put<float>(p,20,9.81f); put<float>(p,16,4500);
    send_packet(sender,port,p);
    require(until([&]{return r->snapshot().active;}),"actual UDP activates receiver");
    require(r->snapshot().values.sway==1&&same->snapshot().values.rpm==4500,"subscribers see same values");
    auto packets=r->snapshot().packets;
    put<float>(p,20,std::numeric_limits<float>::quiet_NaN()); send_packet(sender,port,p);
    std::this_thread::sleep_for(milliseconds(40)); require(r->snapshot().packets==packets,"malformed packet not published");
    require(until([&]{return !r->snapshot().active;},900),"500ms stale timeout");
    require(r->snapshot().age_seconds>=0.5,"stale age reported");
    put<float>(p,20,19.62f); send_packet(sender,port,p); require(until([&]{return r->snapshot().active;}),"fresh data recovers");
    put<int32_t>(p,0,0); send_packet(sender,port,p);
    require(until([&]{return !r->snapshot().active;},300),"inactive packet clears active immediately");
    auto start=steady_clock::now(); same.reset(); r.reset();
    require(duration_cast<milliseconds>(steady_clock::now()-start).count()<500,"last subscriber shutdown bounded");
    require(until([&]{return conflict->snapshot().status.find("Listening")!=std::string::npos;},2000),"bind conflict automatically recovers when owner releases port");
    conflict.reset();
    auto reopened=Receiver::acquire(config);
    require(until([&]{return reopened->snapshot().status.find("Listening")!=std::string::npos;}),"receiver weak registry releases socket");
    reopened.reset();
    const uint16_t f1_port=free_port();
    auto f1_receiver=Receiver::acquire({Game::F1,f1_port,false});
    require(until([&]{return f1_receiver->snapshot().status.find("Listening")!=std::string::npos;}),"F1 listener ready");
    std::vector<uint8_t> f1(29+22*60+3); put<uint16_t>(f1,0,2025); f1[5]=1; f1[27]=1;
    put<float>(f1,29+60+36,1.25f); send_packet(sender,f1_port,f1);
    require(until([&]{return f1_receiver->snapshot().active;}),"F1 motion activates receiver");
    f1[6]=6; put<uint16_t>(f1,29+60,155);
    auto auxiliary_end=steady_clock::now()+milliseconds(650);
    while(steady_clock::now()<auxiliary_end) { send_packet(sender,f1_port,f1); std::this_thread::sleep_for(milliseconds(30)); }
    require(!f1_receiver->snapshot().active&&f1_receiver->snapshot().values.speed==155,"F1 auxiliary updates speed without refreshing stale motion");
    f1_receiver.reset(); closesocket(sender);

    HANDLE ph=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,220,L"Local\\acpmf_physics");
    bool own_ph=ph && GetLastError()!=ERROR_ALREADY_EXISTS;
    HANDLE gh=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,8,L"Local\\acpmf_graphics");
    bool own_gh=gh && GetLastError()!=ERROR_ALREADY_EXISTS;
    if(own_ph&&own_gh) {
        auto *pv=static_cast<uint8_t*>(MapViewOfFile(ph,FILE_MAP_WRITE,0,0,220));
        auto *gv=static_cast<uint8_t*>(MapViewOfFile(gh,FILE_MAP_WRITE,0,0,8));
        require(pv&&gv,"AC fixture mappings");
        std::vector<uint8_t> phys(220),gfx(8); put<int32_t>(phys,0,1); put<float>(phys,44,2.5f); put<int32_t>(gfx,4,2);
        std::memcpy(pv,phys.data(),phys.size()); std::memcpy(gv,gfx.data(),gfx.size());
        auto ac=Receiver::acquire({Game::AC,0,false});
        require(until([&]{return ac->snapshot().active;}),"actual AC shared mapping read");
        require(ac->snapshot().values.sway==2.5f,"AC mapped value");
        require(until([&]{return !ac->snapshot().active;},900),"unchanged AC packet goes stale");
        put<int32_t>(phys,0,2); std::memcpy(pv,phys.data(),phys.size());
        require(until([&]{return ac->snapshot().active;}),"AC new packet resumes");
        put<int32_t>(gfx,4,1); std::memcpy(gv,gfx.data(),gfx.size());
        require(until([&]{return !ac->snapshot().active;},300),"AC inactive graphics clears active without new physics");
        ac.reset(); UnmapViewOfFile(pv); UnmapViewOfFile(gv);
    } else std::cout<<"AC mapping test skipped: game-owned map already exists\n";
    if(ph) CloseHandle(ph); if(gh) CloseHandle(gh);

    // SCS adapter uses its own synchronized protocol and must convert m/s² to G.
    HANDLE tm=CreateMutexW(nullptr,FALSE,truck::mutex_name(false));
    bool own_tm=tm && GetLastError()!=ERROR_ALREADY_EXISTS;
    HANDLE th=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,static_cast<DWORD>(truck::kMappingSize),truck::mapping_name(false));
    bool own_th=th && GetLastError()!=ERROR_ALREADY_EXISTS;
    if(own_tm&&own_th) {
        auto *tv=static_cast<truck::Snapshot*>(MapViewOfFile(th,FILE_MAP_WRITE,0,0,truck::kMappingSize));
        require(tv!=nullptr,"truck fixture mapping");
        truck::Snapshot packet{};
        packet.magic=truck::kMagic; packet.version=truck::kVersion; packet.size=truck::kMappingSize;
        packet.active=1; packet.sample_counter=1; packet.timestamp_ms=GetTickCount64()-1000;
        packet.sway=9.81f; packet.surge=-19.62f; packet.heave=4.905f;
        auto write=[&] {
            require(WaitForSingleObject(tm,100)==WAIT_OBJECT_0,"truck fixture mutex");
            std::memcpy(tv,&packet,sizeof packet); ReleaseMutex(tm);
        };
        write(); auto truck_receiver=Receiver::acquire({Game::ETS2,0,false});
        std::this_thread::sleep_for(milliseconds(60)); require(!truck_receiver->snapshot().active,"old truck timestamp rejected");
        ++packet.sample_counter; packet.timestamp_ms=GetTickCount64(); write();
        require(until([&]{return truck_receiver->snapshot().active;}),"truck mapping activates receiver");
        auto truck_values=truck_receiver->snapshot().values;
        require(std::abs(truck_values.sway-1)<0.0001f&&std::abs(truck_values.surge+2)<0.0001f&&std::abs(truck_values.heave-0.5f)<0.0001f,"truck acceleration converted to G");
        require(until([&]{return !truck_receiver->snapshot().active;},900),"duplicate truck sample times out");
        packet.timestamp_ms=GetTickCount64(); write();
        std::this_thread::sleep_for(milliseconds(40)); require(!truck_receiver->snapshot().active,"new timestamp without sample counter does not freshen truck");
        ++packet.sample_counter; packet.sway=std::numeric_limits<float>::infinity(); write();
        std::this_thread::sleep_for(milliseconds(40)); require(!truck_receiver->snapshot().active,"truck nonfinite data rejected");
        ++packet.sample_counter; packet.sway=9.81f; packet.timestamp_ms=GetTickCount64(); write();
        require(until([&]{return truck_receiver->snapshot().active;}),"truck resumes on valid new sample");
        packet.active=0; write();
        require(until([&]{return !truck_receiver->snapshot().active;},300),"truck inactive clears motion immediately");
        truck_receiver.reset(); UnmapViewOfFile(tv);
    } else std::cout<<"Truck mapping test skipped: SDK-owned map already exists\n";
    if(th) CloseHandle(th); if(tm) CloseHandle(tm);
    WSACleanup(); std::cout<<"receiver transport, sharing, timeout and shutdown passed\n";
}
