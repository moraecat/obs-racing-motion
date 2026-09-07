#pragma once
#include "telemetry.hpp"
#include <memory>
#include <string>
namespace racing {
struct InputConfig { Game game=Game::Forza; uint16_t port=5300; bool lan=false; };
struct Snapshot {
    Telemetry values{};
    bool active=false;
    double age_seconds=1e9;
    uint64_t packets=0;
    std::string status;
};
class Receiver {
public:
    static std::shared_ptr<Receiver> acquire(InputConfig config);
    Snapshot snapshot() const;
    ~Receiver();
    Receiver(const Receiver&)=delete;
    Receiver &operator=(const Receiver&)=delete;
private:
    explicit Receiver(InputConfig config);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
