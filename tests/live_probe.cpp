// Observes real ETS2 telemetry only. Never creates or writes game mappings.
#include "receiver.hpp"
#include "motion.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>

int main(int argc,char**argv) {
    const int duration=argc>1?std::clamp(std::atoi(argv[1]),1,600):10;
    const char *file=argc>2?argv[2]:"build/live-ets2.csv";
    std::ofstream csv(file);
    if(!csv) return 2;
    csv<<"seconds,active,age,packets,speed,rpm,gear,surge,sway,heave,roll,x,y,rotation\n";
    racing::InputConfig input;input.game=racing::Game::ETS2;
    auto receiver=racing::Receiver::acquire(input);
    racing::Motion motion;racing::MotionSettings settings;
    using Clock=std::chrono::steady_clock;
    const auto start=Clock::now();auto previous=start;
    int logged=-1;unsigned active=0;float max_speed=0,max_x=0,max_y=0,max_roll=0;
    while(true) {
        const auto now=Clock::now();const double elapsed=std::chrono::duration<double>(now-start).count();
        if(elapsed>=duration)break;
        auto s=receiver->snapshot();const float dt=std::chrono::duration<float>(now-previous).count();previous=now;
        const auto pose=motion.step(s.values,s.active,dt,settings);
        csv<<elapsed<<','<<s.active<<','<<s.age_seconds<<','<<s.packets<<','<<s.values.speed<<','<<s.values.rpm<<','<<s.values.gear<<','
           <<s.values.surge<<','<<s.values.sway<<','<<s.values.heave<<','<<s.values.roll<<','<<pose.x<<','<<pose.y<<','<<pose.rotation<<'\n';
        active+=s.active;max_speed=std::max(max_speed,s.values.speed);
        max_x=std::max(max_x,std::abs(pose.x));max_y=std::max(max_y,std::abs(pose.y));max_roll=std::max(max_roll,std::abs(pose.rotation));
        if(int(elapsed)!=logged) {
            logged=int(elapsed);csv.flush();
            std::printf("%ds active=%d packets=%llu speed=%.2f rpm=%.0f G=(%.3f,%.3f,%.3f) %s\n",logged,s.active,
                static_cast<unsigned long long>(s.packets),s.values.speed,s.values.rpm,s.values.surge,s.values.sway,s.values.heave,s.status.c_str());
            std::fflush(stdout);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    std::printf("SUMMARY active_samples=%u max_speed=%.3f max_pose=(%.3fpx,%.3fpx,%.3fdeg)\n",active,max_speed,max_x,max_y,max_roll);
    return active?0:3;
}
