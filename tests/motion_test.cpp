#include "motion.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <algorithm>

static void check(bool condition,const char *what) {
    if(!condition) { std::fprintf(stderr,"FAIL: %s\n",what); std::exit(1); }
}
int main() {
    using namespace racing;
    MotionSettings s; s.smoothing_ms=0;
    Telemetry t; t.sway=1.5f;
    Motion m;
    auto p=m.step(t,true,1.f/60,s);
    check(p.x>10,"lateral acceleration moves the image");
    t.sway=10000; t.heave=10000; t.roll=10000;
    p=m.step(t,true,1.f/60,s);
    check(std::abs(p.x)<=s.x_limit && std::abs(p.y)<=s.y_limit && std::abs(p.rotation)<=s.rotation_limit,"spikes remain inside limits");
    s.intensity=0;
    p=m.step(t,true,1.f/60,s);
    check(p.x==0 && p.y==0 && p.rotation==0,"zero intensity stops motion");
    s.intensity=1; s.smoothing_ms=100; s.return_ms=150;
    t={}; t.sway=1;
    Motion a,b;
    Pose pa,pb;
    for(int i=0;i<30;++i) pa=a.step(t,true,1.f/30,s);
    for(int i=0;i<120;++i) pb=b.step(t,true,1.f/120,s);
    check(std::abs(pa.x-pb.x)<0.05f,"smoothing independent of OBS frame rate");
    for(int i=0;i<120;++i) pa=a.step(t,false,1.f/60,s);
    check(std::abs(pa.x)<0.001 && std::abs(pa.rotation)<0.001,"lost telemetry returns home");
    s.smoothing_ms=0; s.invert_x=true;
    p=m.step(t,true,1.f/60,s);
    check(p.x<0,"inversion changes direction");
    t.sway=NAN;
    p=m.step(t,true,1.f/60,s);
    check(std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.rotation),"nonfinite input never poisons motion state");
    Motion gravity; t={};t.heave=1;
    for(int i=0;i<120;++i) p=gravity.step(t,true,1.f/60,s);
    check(std::abs(p.y)<0.01,"constant gravity does not displace source");
    t.heave=2;
    p=gravity.step(t,true,1.f/60,s);
    check(std::abs(p.y)>1,"road bump changes vertical position");
    // An omitted surge mapping, wrong sign or scale-as-offset breaks these
    // driver-visible acceleration/braking and neutral contracts.
    Motion depth; s=MotionSettings{};s.smoothing_ms=0;t={};t.surge=0.5f;
    p=depth.step(t,true,1.f/60,s);
    check(std::abs(p.scale-0.95f)<0.00001f && std::abs(p.y+12)<0.001f,
          "acceleration moves away and upward");
    t.surge=-0.5f;p=depth.step(t,true,1.f/60,s);
    check(std::abs(p.scale-1.05f)<0.00001f && std::abs(p.y-12)<0.001f,
          "braking moves toward and downward");
    s.invert_surge=true;p=depth.step(t,true,1.f/60,s);
    check(p.scale<1 && p.y<0,"surge inversion reverses both depth cues");
    s.invert_surge=false;s.intensity=0.5f;t.surge=0.5f;p=depth.step(t,true,1.f/60,s);
    check(std::abs(p.scale-0.975f)<0.00001f && std::abs(p.y+6)<0.001f,"overall intensity controls both surge cues");
    s.intensity=2;t.surge=10000;p=depth.step(t,true,1.f/60,s);
    check(p.scale>=0.95f && std::abs(p.y)<=12,"surge spikes obey both limits");
    s.smoothing_ms=1000;s.surge_limit=0.01f;s.surge_y_limit=2;
    p=depth.step(t,true,1.f/60,s);
    check(p.scale>=0.99f && std::abs(p.y)<=2,"lower surge limits clamp immediately");
    s.intensity=0;p=depth.step(t,true,1.f/60,s);
    check(p.scale==1 && p.y==0,"zero intensity immediately restores scale and position");
    s=MotionSettings{};s.smoothing_ms=0;s.surge_limit=0;s.surge_y_limit=0;
    p=depth.step(t,true,1.f/60,s);
    check(p.scale==1 && p.y==0,"zero surge settings preserve original motion");
    s=MotionSettings{};s.smoothing_ms=0;t={};depth=Motion{};
    depth.step(t,true,1.f/60,s);t.surge=0.5f;t.heave=2;
    p=depth.step(t,true,1.f/60,s);
    check(p.y<-12 && p.y>=-28,"road bumps and surge displacement combine within total limit");
    s.invert_y=true;p=depth.step(t,true,1.f/60,s);
    check(p.y>12 && p.scale<1,"vertical inversion affects combined movement without changing scale");
    s=MotionSettings{};t={};t.surge=-0.5f;Motion da,db;
    for(int i=0;i<30;++i) pa=da.step(t,true,1.f/30,s);
    for(int i=0;i<120;++i) pb=db.step(t,true,1.f/120,s);
    check(std::abs(pa.scale-pb.scale)<0.00001f && std::abs(pa.y-pb.y)<0.001f,"surge smoothing independent of frame rate");
    for(int i=0;i<180;++i) pa=da.step(t,false,1.f/60,s);
    check(std::abs(pa.scale-1)<0.00001f && std::abs(pa.y)<0.001f,"stale telemetry restores original size and position");
    s.return_ms=0;t.surge=NAN;p=db.step(t,true,1.f/60,s);
    check(p.scale==1 && p.y==0,"nonfinite surge returns to neutral");
    // Small truck inputs must become visible without expanding excursion limits.
    Motion normal,boosted;s=MotionSettings{};s.smoothing_ms=0;t={};t.heave=1;t.roll=0.2f;
    normal.step(t,true,1.f/60,s);boosted.step(t,true,1.f/60,s);
    t.sway=0.03f;t.surge=0.02f;t.heave=1.01f;t.roll=0.21f;
    pa=normal.step(t,true,1.f/60,s);s.input_gain=5;
    pb=boosted.step(t,true,1.f/60,s);
    check(std::abs(pb.x-pa.x*5)<0.00001f,"input gain amplifies small lateral inputs");
    check(std::abs(pb.y-pa.y*5)<0.0001f,"input gain amplifies road bumps and surge displacement");
    check(std::abs(pb.rotation-pa.rotation*5)<0.00001f,"input gain amplifies combined sway and roll rotation");
    check(std::abs((pb.scale-1)-(pa.scale-1)*5)<0.00001f,"input gain amplifies surge zoom around neutral scale");
    s.input_gain=20;t.sway=30;t.surge=30;t.heave=30;t.roll=6;
    p=boosted.step(t,true,1.f/60,s);
    check(std::abs(p.x)<=s.x_limit && std::abs(p.y)<=s.y_limit+s.surge_y_limit && std::abs(p.rotation)<=s.rotation_limit && p.scale>=1-s.surge_limit,"maximum gain preserves every excursion limit");
    s.intensity=0;p=boosted.step(t,true,1.f/60,s);
    check(p.x==0 && p.y==0 && p.rotation==0 && p.scale==1,"zero intensity overrides maximum input gain");
    s.intensity=1;s.return_ms=0;boosted.step(t,true,1.f/60,s);p=boosted.step(t,false,1.f/60,s);
    check(p.x==0 && p.y==0 && p.rotation==0 && p.scale==1,"stale input returns to neutral at maximum gain");
    // Gain must act after baseline removal; changing it at rest cannot add a bump.
    Motion resting;s.input_gain=1;t={};t.heave=1;t.roll=0.2f;
    resting.step(t,true,1.f/60,s);s.input_gain=20;p=resting.step(t,true,1.f/60,s);
    check(p.x==0 && p.y==0 && p.rotation==0 && p.scale==1,"gain change preserves settled gravity and roll baseline");
    t={};t.sway=0.03f;s.input_gain=1;Motion baseline;pa=baseline.step(t,true,1.f/60,s);
    for(float invalid : {NAN,INFINITY,-10.f,0.f}) {
        Motion invalid_gain;s.input_gain=invalid;pb=invalid_gain.step(t,true,1.f/60,s);
        check(std::abs(pb.x-pa.x)<0.00001f,"invalid or subminimum gain falls back to one");
    }
    s.input_gain=20;Motion capped;pa=capped.step(t,true,1.f/60,s);
    s.input_gain=1000;Motion oversized;pb=oversized.step(t,true,1.f/60,s);
    check(std::abs(pa.x-pb.x)<0.00001f,"out-of-range gain is capped at twenty");
    // Real tiny oscillations must remain visible through normal smoothing.
    Motion vibration_normal,vibration_sensitive;s=MotionSettings{};t={};
    float normal_peak=0,sensitive_peak=0;
    for(int i=0;i<240;++i) {
        t.sway=0.001f*std::sin(i/120.f*8*6.2831853f);
        s.micro_sensitivity=0;pa=vibration_normal.step(t,true,1.f/120,s);
        s.micro_sensitivity=1;pb=vibration_sensitive.step(t,true,1.f/120,s);
        if(i>120){normal_peak=std::max(normal_peak,std::abs(pa.x));sensitive_peak=std::max(sensitive_peak,std::abs(pb.x));}
    }
    check(sensitive_peak>normal_peak*8,"micro sensitivity reveals 0.001 G 8 Hz vibration through 100 ms smoothing");
    s.smoothing_ms=0;s.return_ms=0;t={};Motion quiet;
    for(int i=0;i<120;++i)p=quiet.step(t,true,1.f/60,s);
    check(p.x==0 && p.y==0 && p.rotation==0 && p.scale==1,"micro sensitivity does not invent idle vibration");
    Motion inclined;t.heave=1;t.roll=0.2f;p=inclined.step(t,true,1.f/60,s);
    check(p.y==0 && p.rotation==0,"micro sensitivity preserves constant gravity and roll baseline");
    t={};
    Motion fine;s.micro_sensitivity=1;fine.step(t,true,1.f/60,s);
    t.sway=0.001f;t.heave=0.001f;t.roll=0.001f;t.surge=0.001f;
    p=fine.step(t,true,1.f/60,s);
    check(p.x>0.15f && p.y<-0.35f && p.rotation>0.03f && p.scale<0.9991f,"micro sensitivity affects all rendered motion axes");
    Motion negative; t={};negative.step(t,true,1.f/60,s);
    t.sway=-0.001f;t.heave=-0.001f;t.roll=-0.001f;t.surge=-0.001f;
    pb=negative.step(t,true,1.f/60,s);
    check(std::abs(pb.x+p.x)<0.00001f && std::abs(pb.y+p.y)<0.00001f && std::abs(pb.rotation+p.rotation)<0.00001f && std::abs(pb.scale+p.scale-2)<0.00001f,"micro response is symmetric around neutral");
    t={};t.sway=1e-9f;p=fine.step(t,true,1.f/60,s);
    check(std::abs(p.x)<1e-5f,"micro response has no jump at zero");
    t.sway=30;t.heave=30;t.surge=30;t.roll=6;s.input_gain=20;s.intensity=2;
    p=fine.step(t,true,1.f/60,s);
    check(std::abs(p.x)<=s.x_limit && std::abs(p.y)<=s.y_limit+s.surge_y_limit && std::abs(p.rotation)<=s.rotation_limit && p.scale>=1-s.surge_limit,"combined maximum gain and micro sensitivity preserve limits");
    p=fine.step(t,false,1.f/60,s);
    check(p.x==0 && p.y==0 && p.rotation==0 && p.scale==1,"micro response returns to neutral when input is stale");
    s.intensity=0;p=fine.step(t,true,1.f/60,s);
    check(p.x==0 && p.y==0 && p.rotation==0 && p.scale==1,"zero intensity overrides micro sensitivity");
    std::puts("PASS: micro vibration response, idle neutrality, symmetry, continuity and limits");
    std::puts("PASS: input gain, small truck signals, baseline stability, bounds and neutral return");
    std::puts("PASS: motion bounds, zero, inversion, smoothing, timeout, finite input, gravity and road bump");
    std::puts("PASS: surge depth and vertical cues, intensity, inversion, limits, combination and neutral return");
}
