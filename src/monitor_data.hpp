#pragma once
#include "receiver.hpp"
#include "motion.hpp"
#include <mutex>
#include <string>

namespace racing {
struct MonitorFrame {
    Snapshot received;
    Telemetry preview_input;
    Pose pose;
    bool preview=false,enabled=true,closed=false;
};
struct MonitorLabels {
    std::string receiving,waiting,preview,disabled,actual,simulation,output;
};
struct MonitorState {
    mutable std::mutex mutex;
    MonitorFrame frame;
    MonitorFrame snapshot() const {std::lock_guard<std::mutex> guard(mutex);return frame;}
};
std::string format_monitor(const MonitorFrame&,const MonitorLabels&);
}
