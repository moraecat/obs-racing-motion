#pragma once
#include <cstddef>
#include <cstdint>

namespace racing {
enum class Game { AC, ACC, Forza, FH6, F1, ETS2, ATS };
struct Telemetry {
    float surge=0, sway=0, heave=0, pitch=0, roll=0, yaw=0, speed=0, rpm=0;
    int gear=0;
};
enum class DecodeResult { Invalid, Ignored, Inactive, Motion, Auxiliary };
// Accelerations are G, angles radians, speed km/h. Invalid input never mutates output.
bool finite(const Telemetry &values);
DecodeResult decode_forza(const uint8_t *data, size_t size, bool fh6, Telemetry &out);
DecodeResult decode_f1(const uint8_t *data, size_t size, Telemetry &out);
DecodeResult decode_ac(const uint8_t *physics, size_t physics_size,
                       const uint8_t *graphics, size_t graphics_size,
                       Telemetry &out, uint32_t &packet_id);
}
