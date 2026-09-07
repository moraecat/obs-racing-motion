#pragma once
#include "telemetry.hpp"
namespace racing {
struct MotionSettings {
    float intensity=1, x_limit=24, y_limit=16, rotation_limit=3;
    float input_gain=1;
    float micro_sensitivity=0;
    float surge_limit=0.05f, surge_y_limit=12;
    float smoothing_ms=100, return_ms=250;
    bool invert_x=false,invert_y=false,invert_rotation=false;
    bool invert_surge=false;
};
struct Pose {float x=0,y=0,rotation=0,scale=1;};
class Motion {
public:
    Pose step(const Telemetry&,bool fresh,float dt,const MotionSettings&);
private:
    Pose pose_{};
    float heave_base_=0,roll_base_=0;
    float heave_y_=0,surge_y_=0;
    bool primed_=false;
};
}
