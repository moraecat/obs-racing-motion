#include "../src/truck_protocol.hpp"
#include "scssdk_telemetry.h"
#include "common/scssdk_telemetry_truck_common_channels.h"
#include "eurotrucks2/scssdk_eut2.h"
#include "amtrucks/scssdk_ats.h"
#include <algorithm>
#include <cmath>

namespace {
using racing::truck::Snapshot;
HANDLE mapping = nullptr, mutex = nullptr, producer = nullptr;
Snapshot* shared = nullptr;
Snapshot frame{};
bool driving = false, in_frame = false;
unsigned received = 0;
uint64_t sequence = 0;
constexpr float tau = 6.2831853071795864769f;
enum Channel : unsigned { Acceleration, Placement, Speed, Rpm, Gear };
constexpr unsigned required = (1u << Acceleration) | (1u << Placement);
Channel channel_contexts[] = {Acceleration, Placement, Speed, Rpm, Gear};

void publish(bool active) noexcept {
    if (!shared || !mutex) return;
    Snapshot output = active ? frame : Snapshot{};
    output.magic = racing::truck::kMagic;
    output.version = racing::truck::kVersion;
    output.size = sizeof(output);
    output.active = active ? 1 : 0;
    output.sample_counter = ++sequence;
    output.timestamp_ms = GetTickCount64();
    DWORD lock = WaitForSingleObject(mutex, 0);
    if (lock != WAIT_OBJECT_0 && lock != WAIT_ABANDONED) return;
    std::memcpy(shared, &output, sizeof(output));
    ReleaseMutex(mutex);
}
void cleanup() noexcept {
    driving = false; in_frame = false; publish(false);
    if (shared) UnmapViewOfFile(shared);
    if (mapping) CloseHandle(mapping);
    if (mutex) CloseHandle(mutex);
    if (producer) CloseHandle(producer);
    shared = nullptr; mapping = mutex = producer = nullptr;
}
bool initialize_mapping(bool ats) noexcept {
    // The named object's lifetime is the ownership token. A mutex would allow
    // recursive acquisition by a duplicate DLL initialized on the same thread.
    producer = CreateEventW(nullptr, TRUE, FALSE, ats ? L"Local\\OBSRacingMotionATS_v1_Producer" : L"Local\\OBSRacingMotionETS2_v1_Producer");
    if (!producer) return false;
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(producer); producer = nullptr; return false; }
    mutex = CreateMutexW(nullptr, FALSE, racing::truck::mutex_name(ats));
    if (mutex) mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                          static_cast<DWORD>(sizeof(Snapshot)), racing::truck::mapping_name(ats));
    if (mapping) shared = static_cast<Snapshot*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(Snapshot)));
    if (!shared) { cleanup(); return false; }
    // Existing readers can keep a mapping alive across game restarts; reuse it.
    sequence = GetTickCount64(); frame = {}; received = 0; publish(false);
    return true;
}
bool bounded(float value, float maximum) noexcept { return std::isfinite(value) && std::fabs(value) <= maximum; }
SCSAPI_VOID on_channel(scs_string_t, scs_u32_t, const scs_value_t* value, scs_context_t context) {
    if (!in_frame || !context) return;
    Channel channel = *static_cast<const Channel*>(context);
    const unsigned bit = 1u << channel;
    received &= ~bit;
    if (!value) return;
    switch (channel) {
    case Acceleration: {
        if (value->type != SCS_VALUE_TYPE_fvector) return;
        const auto& a = value->value_fvector;
        if (!bounded(a.x, 500) || !bounded(a.y, 500) || !bounded(a.z, 500)) return;
        frame.surge = -a.z; frame.sway = a.x; frame.heave = a.y;
        break;
    }
    case Placement: {
        if (value->type != SCS_VALUE_TYPE_dplacement) return;
        const auto& o = value->value_dplacement.orientation;
        if (!bounded(o.heading, 1) || !bounded(o.pitch, .25f) || !bounded(o.roll, .5f)) return;
        frame.pitch = o.pitch * tau; frame.roll = o.roll * tau;
        frame.yaw = std::remainder(o.heading, 1.0f) * tau;
        break;
    }
    case Speed:
        if (value->type != SCS_VALUE_TYPE_float || !bounded(value->value_float.value, 500)) return;
        frame.speed = std::fabs(value->value_float.value) * 3.6f; break;
    case Rpm:
        if (value->type != SCS_VALUE_TYPE_float || !bounded(value->value_float.value, 50000)) return;
        frame.rpm = std::max(0.0f, value->value_float.value); break;
    case Gear:
        if (value->type != SCS_VALUE_TYPE_s32 || value->value_s32.value < -32 || value->value_s32.value > 32) return;
        frame.gear = value->value_s32.value; break;
    }
    received |= bit;
}
SCSAPI_VOID on_event(scs_event_t event, const void*, scs_context_t) {
    switch (event) {
    case SCS_TELEMETRY_EVENT_started: driving = true; break;
    case SCS_TELEMETRY_EVENT_paused: driving = false; in_frame = false; publish(false); break;
    case SCS_TELEMETRY_EVENT_frame_start: frame = {}; received = 0; in_frame = true; break;
    case SCS_TELEMETRY_EVENT_frame_end:
        publish(driving && in_frame && (received & required) == required); in_frame = false; break;
    }
}
} // namespace

extern "C" SCSAPI_RESULT scs_telemetry_init(scs_u32_t version, const scs_telemetry_init_params_t* params) {
    if (version != SCS_TELEMETRY_VERSION_1_00 && version != SCS_TELEMETRY_VERSION_1_01) return SCS_RESULT_unsupported;
    if (!params) return SCS_RESULT_invalid_parameter;
    if (mapping) return SCS_RESULT_already_registered;
    auto p = static_cast<const scs_telemetry_init_params_v100_t*>(params);
    if (!p->common.game_id || !p->register_for_event || !p->register_for_channel) return SCS_RESULT_invalid_parameter;
    bool ats = std::strcmp(p->common.game_id, SCS_GAME_ID_ATS) == 0;
    if (!ats && std::strcmp(p->common.game_id, SCS_GAME_ID_EUT2) != 0) return SCS_RESULT_unsupported;
    const scs_event_t events[] = {SCS_TELEMETRY_EVENT_frame_start, SCS_TELEMETRY_EVENT_frame_end,
                                 SCS_TELEMETRY_EVENT_paused, SCS_TELEMETRY_EVENT_started};
    for (auto event : events) if (p->register_for_event(event, on_event, nullptr) != SCS_RESULT_ok) return SCS_RESULT_generic_error;
    const char* names[] = {SCS_TELEMETRY_TRUCK_CHANNEL_local_linear_acceleration,
                          SCS_TELEMETRY_TRUCK_CHANNEL_world_placement, SCS_TELEMETRY_TRUCK_CHANNEL_speed,
                          SCS_TELEMETRY_TRUCK_CHANNEL_engine_rpm, SCS_TELEMETRY_TRUCK_CHANNEL_engine_gear};
    const scs_value_type_t types[] = {SCS_VALUE_TYPE_fvector, SCS_VALUE_TYPE_dplacement, SCS_VALUE_TYPE_float, SCS_VALUE_TYPE_float, SCS_VALUE_TYPE_s32};
    for (unsigned i = 0; i < 5; ++i) {
        auto result = p->register_for_channel(names[i], SCS_U32_NIL, types[i],
            SCS_TELEMETRY_CHANNEL_FLAG_each_frame | SCS_TELEMETRY_CHANNEL_FLAG_no_value, on_channel, &channel_contexts[i]);
        if (result != SCS_RESULT_ok && i <= Placement) return SCS_RESULT_generic_error;
    }
    if (!initialize_mapping(ats)) return SCS_RESULT_generic_error;
    if (p->common.log) p->common.log(SCS_LOG_TYPE_message, "OBS Racing Motion: native truck telemetry initialized");
    return SCS_RESULT_ok;
}
extern "C" SCSAPI_VOID scs_telemetry_shutdown() { cleanup(); }
