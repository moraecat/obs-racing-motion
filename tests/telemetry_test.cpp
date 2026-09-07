#include "telemetry.hpp"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>
using namespace racing;
static void require(bool value, const char *why) { if (!value) { std::cerr << "FAIL: " << why << '\n'; std::exit(1); } }
static bool near(float a,float b) { return std::abs(a-b)<0.0001f; }
template<class T> static void put(std::vector<uint8_t>&b,size_t p,T v) { std::memcpy(b.data()+p,&v,sizeof v); }
int main() {
    Telemetry t{};
    std::vector<uint8_t> forza(232);
    put<int32_t>(forza,0,1); put<float>(forza,16,7000);
    put<float>(forza,20,9.81f); put<float>(forza,24,-19.62f); put<float>(forza,28,29.43f);
    put<float>(forza,32,3); put<float>(forza,36,4);
    put<float>(forza,56,0.1f); put<float>(forza,60,0.2f); put<float>(forza,64,0.3f);
    require(decode_forza(forza.data(),forza.size(),false,t)==DecodeResult::Motion,"Forza Sled accepted");
    require(near(t.sway,1)&&near(t.heave,-2)&&near(t.surge,3),"Forza G conversion and axes");
    require(near(t.speed,18)&&t.rpm==7000&&near(t.pitch,0.2f)&&near(t.yaw,0.1f)&&near(t.roll,0.3f),"Forza units and angles");
    forza.resize(324); put<float>(forza,244,1234);
    require(decode_forza(forza.data(),forza.size(),false,t)==DecodeResult::Motion&&near(t.speed,18),"generic Forza uses layout-independent velocity speed");
    forza.resize(336); put<float>(forza,256,25); forza[319]=6;
    require(decode_forza(forza.data(),forza.size(),true,t)==DecodeResult::Motion&&near(t.speed,90)&&t.gear==6,"FH6 custom Dash offsets");
    put<int32_t>(forza,0,0);
    require(decode_forza(forza.data(),forza.size(),false,t)==DecodeResult::Inactive,"inactive race explicit");
    put<int32_t>(forza,0,1); put<float>(forza,20,std::numeric_limits<float>::quiet_NaN());
    auto before=t;
    require(decode_forza(forza.data(),forza.size(),false,t)==DecodeResult::Invalid&&t.speed==before.speed,"NaN rejected without partial output");
    require(decode_forza(forza.data(),231,false,t)==DecodeResult::Invalid,"truncated Forza rejected");
    require(decode_forza(nullptr,0,false,t)==DecodeResult::Invalid,"empty packet rejected");

    for(uint16_t year: {2023,2024,2025}) {
        std::vector<uint8_t> f1(29+22*60+3);
        put<uint16_t>(f1,0,year); f1[5]=1; f1[27]=3;
        size_t base=29+3*60;
        put<float>(f1,base+36,1.5f); put<float>(f1,base+40,-2.5f); put<float>(f1,base+44,0.5f);
        put<float>(f1,base+48,0.4f); put<float>(f1,base+52,0.6f); put<float>(f1,base+56,-0.2f);
        require(decode_f1(f1.data(),f1.size(),t)==DecodeResult::Motion,"supported F1 version motion");
        require(near(t.sway,1.5f)&&near(t.surge,-2.5f)&&near(t.heave,0.5f)&&near(t.pitch,0.6f),"F1 selected car and G values");
        f1[6]=6; put<uint16_t>(f1,base,212); f1[base+15]=255; put<uint16_t>(f1,base+16,11500);
        require(decode_f1(f1.data(),f1.size(),t)==DecodeResult::Auxiliary&&t.speed==212&&t.gear==-1&&t.rpm==11500,"F1 telemetry preserves reverse gear");
        require(near(t.sway,1.5f),"auxiliary F1 preserves motion");
        f1[27]=22; require(decode_f1(f1.data(),f1.size(),t)==DecodeResult::Invalid,"out-of-range F1 player rejected");
        f1[27]=3; f1[5]=2; require(decode_f1(f1.data(),f1.size(),t)==DecodeResult::Invalid,"unknown packet version rejected");
        f1[5]=1; put<uint16_t>(f1,0,2022); require(decode_f1(f1.data(),f1.size(),t)==DecodeResult::Invalid,"unsupported F1 year rejected");
        put<uint16_t>(f1,0,year); require(decode_f1(f1.data(),40,t)==DecodeResult::Invalid,"truncated F1 rejected");
        f1[6]=0; put<float>(f1,base+36,std::numeric_limits<float>::infinity());
        require(decode_f1(f1.data(),f1.size(),t)==DecodeResult::Invalid,"nonfinite F1 rejected");
    }

    std::vector<uint8_t> p(220),g(8); uint32_t id=0;
    put<int32_t>(p,0,73); put<int32_t>(g,4,2);
    put<int32_t>(p,16,4); put<int32_t>(p,20,6500); put<float>(p,28,120);
    put<float>(p,44,1); put<float>(p,48,2); put<float>(p,52,3);
    put<float>(p,208,0.25f); put<float>(p,212,0.5f); put<float>(p,216,0.75f);
    require(decode_ac(p.data(),p.size(),g.data(),g.size(),t,id)==DecodeResult::Motion,"AC physics prefix accepted");
    require(id==73&&t.sway==1&&t.heave==2&&t.surge==3&&t.speed==120&&t.rpm==6500&&t.gear==4&&t.roll==0.75f,"AC real offsets");
    put<int32_t>(g,4,1); require(decode_ac(p.data(),p.size(),g.data(),g.size(),t,id)==DecodeResult::Inactive,"AC replay inactive");
    put<int32_t>(g,4,2); put<float>(p,212,std::numeric_limits<float>::quiet_NaN());
    require(decode_ac(p.data(),p.size(),g.data(),g.size(),t,id)==DecodeResult::Invalid,"AC NaN rejected");
    require(decode_ac(p.data(),216,g.data(),8,t,id)==DecodeResult::Invalid,"AC short mapping rejected");
    std::cout << "telemetry codecs passed\n";
}
