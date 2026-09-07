#include "monitor_data.hpp"
#include <cmath>
#include <cstdio>
namespace racing {
namespace {
std::string axes(const Telemetry &t) {
    char line[512];
    std::snprintf(line,sizeof(line),
        "Surge %.2f G    Sway %.2f G    Heave %.2f G\r\n"
        "Pitch %.2f deg    Roll %.2f deg    Yaw %.2f deg\r\n"
        "%.0f km/h    %.0f RPM    Gear %d\r\n",
        t.surge,t.sway,t.heave,t.pitch*57.295779513f,t.roll*57.295779513f,t.yaw*57.295779513f,t.speed,t.rpm,t.gear);
    return line;
}
}
std::string format_monitor(const MonitorFrame &frame,const MonitorLabels &labels) {
    const auto &s=frame.received;
    const bool fresh=s.active && std::isfinite(s.age_seconds) && s.age_seconds<0.5;
    std::string result=labels.actual+" — "+(fresh?labels.receiving:labels.waiting)+"\r\n";
    result+=s.status+"\r\n"+axes(s.values);
    char line[256];
    if(s.packets && std::isfinite(s.age_seconds) && s.age_seconds>=0)
        std::snprintf(line,sizeof(line),"Age %.0f ms    %llu packets\r\n",s.age_seconds*1000,static_cast<unsigned long long>(s.packets));
    else std::snprintf(line,sizeof(line),"Age --    %llu packets\r\n",static_cast<unsigned long long>(s.packets));
    result+=line;
    if(frame.preview) result+="\r\n"+labels.simulation+" — "+labels.preview+"\r\n"+axes(frame.preview_input);
    result+="\r\n"+labels.output+(frame.enabled?"":" — "+labels.disabled)+"\r\n";
    std::snprintf(line,sizeof(line),"X %.2f px    Y %.2f px\r\nRotation %.2f deg    Scale %.1f%%\r\n",frame.pose.x,frame.pose.y,frame.pose.rotation,frame.pose.scale*100);
    return result+line;
}
}
