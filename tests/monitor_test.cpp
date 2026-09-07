#include "monitor_data.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
void check(bool pass,const char *message) {if(!pass){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
int main() {
    using namespace racing;
    MonitorLabels labels{"LIVE","WAITING","PREVIEW","DISABLED","ACTUAL","SIMULATED","OUTPUT"};
    MonitorFrame frame;frame.received.active=true;frame.received.age_seconds=0.012;
    frame.received.packets=42;
    frame.received.values={0.25f,-0.5f,1.0f,0.174532925f,-0.34906585f,1.570796327f,80,1500,6};
    auto display=format_monitor(frame,labels);
    check(display.find("LIVE")!=std::string::npos,"fresh active telemetry displays live status");
    check(display.find("Surge 0.25 G")!=std::string::npos && display.find("Sway -0.50 G")!=std::string::npos && display.find("Heave 1.00 G")!=std::string::npos,"received acceleration units and signs visible");
    check(display.find("Pitch 10.00 deg")!=std::string::npos && display.find("Roll -20.00 deg")!=std::string::npos && display.find("Yaw 90.00 deg")!=std::string::npos,"all three rotations converted to degrees");
    check(display.find("80 km/h")!=std::string::npos && display.find("1500 RPM")!=std::string::npos && display.find("Gear 6")!=std::string::npos,"driving gauges visible");
    check(display.find("12 ms")!=std::string::npos && display.find("42 packets")!=std::string::npos,"packet age and count visible");
    frame.received.age_seconds=0.6;display=format_monitor(frame,labels);
    check(display.find("LIVE")==std::string::npos && display.find("WAITING")!=std::string::npos,"stale active flag is not displayed as live");
    frame.preview=true;frame.preview_input.surge=0.5f;frame.pose.scale=0.95f;
    display=format_monitor(frame,labels);
    check(display.find("ACTUAL")!=std::string::npos && display.find("SIMULATED")!=std::string::npos && display.find("Surge 0.50 G")!=std::string::npos && display.find("Surge 0.25 G")!=std::string::npos,"preview input is separated from last actual data");
    check(display.find("95.0%")!=std::string::npos,"applied scale uses percent instead of offset");
    frame.enabled=false;check(format_monitor(frame,labels).find("DISABLED")!=std::string::npos,"disabled filter indicated even if receiving");
    frame={};check(format_monitor(frame,labels).find("Age --")!=std::string::npos,"no sample does not present a fabricated age");
    std::puts("PASS: live telemetry display, axes/units, freshness, preview separation and applied motion");
}
