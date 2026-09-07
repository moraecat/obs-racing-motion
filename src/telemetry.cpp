#include "telemetry.hpp"
#include <cmath>
#include <cstring>
namespace racing {
namespace {
template<class T> T read(const uint8_t *data,size_t offset) {
    T value; std::memcpy(&value,data+offset,sizeof value); return value;
}
}
bool finite(const Telemetry &v) {
    return std::isfinite(v.surge)&&std::isfinite(v.sway)&&std::isfinite(v.heave)&&
           std::isfinite(v.pitch)&&std::isfinite(v.roll)&&std::isfinite(v.yaw)&&
           std::isfinite(v.speed)&&std::isfinite(v.rpm);
}
DecodeResult decode_forza(const uint8_t *data,size_t size,bool fh6,Telemetry &out) {
    if (!data || size<232) return DecodeResult::Invalid;
    if (read<int32_t>(data,0)==0) return DecodeResult::Inactive;
    Telemetry v{};
    v.rpm=read<float>(data,16);
    v.sway=read<float>(data,20)/9.81f; v.heave=read<float>(data,24)/9.81f; v.surge=read<float>(data,28)/9.81f;
    const double vx=read<float>(data,32),vy=read<float>(data,36),vz=read<float>(data,40);
    v.speed=static_cast<float>(std::sqrt(vx*vx+vy*vy+vz*vz)*3.6);
    v.yaw=read<float>(data,56); v.pitch=read<float>(data,60); v.roll=read<float>(data,64);
    // Preserve the user's FH6 bridge layout; generic Forza Dash layouts vary.
    if (fh6 && size>=336) { v.speed=read<float>(data,256)*3.6f; v.gear=data[319]; }
    if (!finite(v)) return DecodeResult::Invalid;
    out=v; return DecodeResult::Motion;
}
DecodeResult decode_f1(const uint8_t *data,size_t size,Telemetry &out) {
    if (!data || size<29) return DecodeResult::Invalid;
    const uint16_t year=read<uint16_t>(data,0);
    if ((year!=2023 && year!=2024 && year!=2025) || data[5]!=1 || data[27]>=22) return DecodeResult::Invalid;
    const unsigned packet=data[6];
    if (packet!=0 && packet!=6) return DecodeResult::Ignored;
    // Both supported packet families carry 22 records of 60 bytes.
    if (size<29+22*60+(packet==6?3:0)) return DecodeResult::Invalid;
    const size_t base=29+data[27]*60;
    Telemetry v=out;
    if (packet==0) {
        v.sway=read<float>(data,base+36); v.surge=read<float>(data,base+40); v.heave=read<float>(data,base+44);
        v.yaw=read<float>(data,base+48); v.pitch=read<float>(data,base+52); v.roll=read<float>(data,base+56);
    } else {
        v.speed=read<uint16_t>(data,base); v.gear=read<int8_t>(data,base+15); v.rpm=read<uint16_t>(data,base+16);
    }
    if (!finite(v)) return DecodeResult::Invalid;
    out=v; return packet==0?DecodeResult::Motion:DecodeResult::Auxiliary;
}
DecodeResult decode_ac(const uint8_t *p,size_t ps,const uint8_t *g,size_t gs,Telemetry &out,uint32_t &id) {
    if (!p || !g || ps<220 || gs<8) return DecodeResult::Invalid;
    if (read<int32_t>(g,4)!=2) return DecodeResult::Inactive;
    Telemetry v{};
    v.gear=read<int32_t>(p,16); v.rpm=static_cast<float>(read<int32_t>(p,20)); v.speed=read<float>(p,28);
    v.sway=read<float>(p,44); v.heave=read<float>(p,48); v.surge=read<float>(p,52);
    v.yaw=read<float>(p,208); v.pitch=read<float>(p,212); v.roll=read<float>(p,216);
    if (!finite(v)) return DecodeResult::Invalid;
    out=v; id=read<uint32_t>(p,0); return DecodeResult::Motion;
}
}
