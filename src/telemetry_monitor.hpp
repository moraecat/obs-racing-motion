#pragma once
#include "monitor_data.hpp"
#include <memory>
#include <string>
namespace racing {
class TelemetryMonitor {
public:
    explicit TelemetryMonitor(std::shared_ptr<MonitorState>);
    ~TelemetryMonitor();
    void open(std::string title,MonitorLabels labels);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
