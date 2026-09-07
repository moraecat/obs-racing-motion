#include "motion.hpp"
#include <algorithm>
#include <cmath>
namespace racing {
namespace {
float bounded(float v,float lo,float hi,float fallback=0) {
    return std::isfinite(v)?std::clamp(v,lo,hi):fallback;
}
float blend(float dt,float ms) {return ms<=0?1.f:1.f-std::exp(-dt*1000.f/ms);}
}
Pose Motion::step(const Telemetry& t,bool fresh,float dt,const MotionSettings& settings) {
    dt=bounded(dt,0,0.25f);
    // Amplify after baseline removal, before saturation and output smoothing.
    const float intensity=bounded(settings.intensity,0,2)*bounded(settings.input_gain,1,20,1);
    if(intensity==0) {pose_={};heave_y_=surge_y_=0;primed_=false;return pose_;}
    const float micro=9*bounded(settings.micro_sensitivity,0,1);
    const auto response=[intensity,micro](float value) {
        const float u=bounded(value*intensity,-1,1);
        // Finite slope at zero, symmetric and continuous; endpoints stay +/-1.
        return micro==0?u:u*(1+micro)/(1+micro*std::abs(u));
    };
    const float sway=bounded(t.sway,-30,30),heave=bounded(t.heave,-30,30);
    const float surge=bounded(t.surge,-30,30);
    const float surge_limit=bounded(settings.surge_limit,0,0.5f);
    const float surge_y_limit=bounded(settings.surge_y_limit,0,500);
    const float roll=bounded(t.roll,-6.3f,6.3f);
    fresh=fresh && std::isfinite(t.sway) && std::isfinite(t.heave) && std::isfinite(t.roll) && std::isfinite(t.surge);
    Pose target;
    float target_surge_y=0;
    if(fresh) {
        if(!primed_) {heave_base_=heave;roll_base_=roll;primed_=true;}
        const float base_alpha=blend(dt,1800);
        heave_base_+=(heave-heave_base_)*base_alpha;
        roll_base_+=(roll-roll_base_)*base_alpha;
        const float lateral=sway/1.5f;
        target.x=response(lateral)*bounded(settings.x_limit,0,500)*(settings.invert_x?-1.f:1.f);
        target.y=response(-(heave-heave_base_))*bounded(settings.y_limit,0,500)*(settings.invert_y?-1.f:1.f);
        // Positive surge is forward acceleration: the subject recedes/rises.
        // Braking reverses the cues. Normalize at 0.5 G for truck driving.
        const float longitudinal=response(-surge/0.5f)*(settings.invert_surge?-1.f:1.f);
        target.scale=1+longitudinal*surge_limit;
        target_surge_y=longitudinal*surge_y_limit*(settings.invert_y?-1.f:1.f);
        target.rotation=response(0.7f*lateral+0.3f*(roll-roll_base_)/0.35f)*bounded(settings.rotation_limit,0,45)*(settings.invert_rotation?-1.f:1.f);
    } else primed_=false;
    const float alpha=blend(dt,bounded(fresh?settings.smoothing_ms:settings.return_ms,0,2000));
    pose_.x+=(target.x-pose_.x)*alpha;
    heave_y_+=(target.y-heave_y_)*alpha;
    surge_y_+=(target_surge_y-surge_y_)*alpha;
    pose_.rotation+=(target.rotation-pose_.rotation)*alpha;
    pose_.scale+=(target.scale-pose_.scale)*alpha;
    // Lowering excursion limits takes effect immediately, even while smoothing.
    pose_.x=bounded(pose_.x,-bounded(settings.x_limit,0,500),bounded(settings.x_limit,0,500));
    heave_y_=bounded(heave_y_,-bounded(settings.y_limit,0,500),bounded(settings.y_limit,0,500));
    surge_y_=bounded(surge_y_,-surge_y_limit,surge_y_limit);
    pose_.y=heave_y_+surge_y_;
    pose_.rotation=bounded(pose_.rotation,-bounded(settings.rotation_limit,0,45),bounded(settings.rotation_limit,0,45));
    pose_.scale=bounded(pose_.scale,1-surge_limit,1+surge_limit,1);
    return pose_;
}
}
