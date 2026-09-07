#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "scssdk_telemetry.h"
#include "common/scssdk_telemetry_truck_common_channels.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <limits>
#include "../src/truck_protocol.hpp"
#include <thread>
#include <atomic>

// Independent wire layout: catches ABI drift rather than sharing writer internals.
struct Snapshot {
    uint32_t magic, version, size, active;
    uint64_t sample_counter, timestamp_ms;
    float surge, sway, heave, pitch, roll, yaw, speed, rpm;
    int32_t gear; uint32_t reserved;
};
struct Channel { scs_telemetry_channel_callback_t callback; scs_context_t context; };
std::map<std::string, Channel> channels;
std::map<scs_event_t, std::pair<scs_telemetry_event_callback_t, scs_context_t>> events;
bool fail_channel = false;
void check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
SCSAPI_RESULT reg_event(scs_event_t e, scs_telemetry_event_callback_t cb, scs_context_t ctx) {
    events[e] = {cb, ctx}; return SCS_RESULT_ok;
}
SCSAPI_RESULT reg_channel(scs_string_t name, scs_u32_t index, scs_value_type_t,
                         scs_u32_t flags, scs_telemetry_channel_callback_t cb, scs_context_t ctx) {
    check(index == SCS_U32_NIL, "scalar index");
    check((flags & SCS_TELEMETRY_CHANNEL_FLAG_each_frame) != 0, "full frame delivery");
    if (fail_channel) return SCS_RESULT_not_found;
    channels[name] = {cb, ctx}; return SCS_RESULT_ok;
}
void emit(scs_event_t e) { auto c = events.at(e); c.first(e, nullptr, c.second); }
void value(const char* name, const scs_value_t* v) {
    auto c = channels.at(name); c.callback(name, SCS_U32_NIL, v, c.context);
}
void float_value(const char* name, float f) {
    scs_value_t v{}; v.type = SCS_VALUE_TYPE_float; v.value_float.value = f; value(name, &v);
}
int main(int argc, char** argv) {
    check(argc == 2, "usage: truck_test.exe DLL-path");
    HMODULE dll = LoadLibraryA(argv[1]); check(dll != nullptr, "game telemetry DLL is loadable");
    auto init = reinterpret_cast<decltype(&scs_telemetry_init)>(GetProcAddress(dll, "scs_telemetry_init"));
    auto shutdown = reinterpret_cast<decltype(&scs_telemetry_shutdown)>(GetProcAddress(dll, "scs_telemetry_shutdown"));
    check(init && shutdown, "SCS exports");
    scs_telemetry_init_params_v100_t params{};
    params.common.game_id = "eut2"; params.register_for_event = reg_event; params.register_for_channel = reg_channel;
    check(init(0xffffffff, &params) == SCS_RESULT_unsupported, "reject unsupported SDK");
    check(init(SCS_TELEMETRY_VERSION_1_01, nullptr) == SCS_RESULT_invalid_parameter, "reject null params");
    params.common.game_id = "unknown";
    check(init(SCS_TELEMETRY_VERSION_1_01, &params) == SCS_RESULT_unsupported, "reject unknown game");
    params.common.game_id = "eut2";
    fail_channel = true;
    check(init(SCS_TELEMETRY_VERSION_1_01, &params) != SCS_RESULT_ok, "registration failure rejects initialization");
    fail_channel = false; channels.clear(); events.clear();
    check(init(SCS_TELEMETRY_VERSION_1_01, &params) == SCS_RESULT_ok, "initialize ETS2");
    const std::string duplicate_path = std::string(argv[1]) + ".duplicate.dll";
    check(CopyFileA(argv[1], duplicate_path.c_str(), FALSE) != FALSE, "copy DLL under distinct filename");
    HMODULE duplicate = LoadLibraryA(duplicate_path.c_str());
    check(duplicate != nullptr && duplicate != dll, "load separate DLL instance");
    auto duplicate_init = reinterpret_cast<decltype(&scs_telemetry_init)>(GetProcAddress(duplicate, "scs_telemetry_init"));
    auto duplicate_shutdown = reinterpret_cast<decltype(&scs_telemetry_shutdown)>(GetProcAddress(duplicate, "scs_telemetry_shutdown"));
    check(duplicate_init && duplicate_shutdown, "duplicate SCS exports");
    // Simulate separate SDK callback registries for independently loaded plugins.
    const auto primary_events = events;
    const auto primary_channels = channels;
    check(duplicate_init(SCS_TELEMETRY_VERSION_1_01, &params) != SCS_RESULT_ok,
          "reject duplicate producer initialized on the same SDK thread");
    events = primary_events; channels = primary_channels;
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Local\\OBSRacingMotionETS2_v1");
    HANDLE mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, L"Local\\OBSRacingMotionETS2_v1_Mutex");
    check(mapping && mutex, "ETS2 shared memory and mutex");
    auto memory = static_cast<const Snapshot*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(Snapshot)));
    check(memory, "map protocol snapshot");
    auto read = [&]() { check(WaitForSingleObject(mutex, 0) == WAIT_OBJECT_0, "snapshot lock");
        Snapshot s = *memory; ReleaseMutex(mutex); return s; };
    Snapshot s = read(); check(s.magic == 0x4d52544f && s.version == 1 && s.size == 72, "protocol identity");
    racing::truck::Reader reader;
    racing::truck::Snapshot wire{};
    check(reader.read(wire) && wire.magic == s.magic, "production reader opens read-only snapshot");
    check(s.active == 0, "initially paused");
    emit(SCS_TELEMETRY_EVENT_started); emit(SCS_TELEMETRY_EVENT_frame_start);
    scs_value_t a{}; a.type = SCS_VALUE_TYPE_fvector; a.value_fvector = {2,3,-4};
    value(SCS_TELEMETRY_TRUCK_CHANNEL_local_linear_acceleration, &a);
    scs_value_t p{}; p.type = SCS_VALUE_TYPE_dplacement; p.value_dplacement.orientation = {.75f,.125f,-.25f};
    value(SCS_TELEMETRY_TRUCK_CHANNEL_world_placement, &p);
    float_value(SCS_TELEMETRY_TRUCK_CHANNEL_speed, 10); float_value(SCS_TELEMETRY_TRUCK_CHANNEL_engine_rpm, 1500);
    scs_value_t g{}; g.type = SCS_VALUE_TYPE_s32; g.value_s32.value = 7;
    value(SCS_TELEMETRY_TRUCK_CHANNEL_engine_gear, &g);
    check(read().active == 0, "no partially published frame");
    emit(SCS_TELEMETRY_EVENT_frame_end); s = read();
    check(s.active == 1 && s.surge == 4 && s.sway == 2 && s.heave == 3, "SCS local axes");
    check(std::fabs(s.pitch - .78539816f) < .00001f && std::fabs(s.roll + 1.5707963f) < .00001f, "turns to radians");
    check(s.speed == 36 && s.rpm == 1500 && s.gear == 7, "dashboard units");
    check(s.timestamp_ms <= GetTickCount64() && GetTickCount64()-s.timestamp_ms < 1000, "heartbeat uses system monotonic clock");
    std::atomic<bool> locked{false}, release{false};
    std::thread holder([&] { WaitForSingleObject(mutex, INFINITE); locked = true;
        while (!release) Sleep(1); ReleaseMutex(mutex); });
    while (!locked) Sleep(1);
    auto before = GetTickCount64();
    check(!reader.read(wire), "reader skips mutex contention");
    emit(SCS_TELEMETRY_EVENT_frame_end);
    check(GetTickCount64() - before < 100, "game callback skips mutex contention without waiting");
    release = true; holder.join();
    std::thread abandoned([&] { WaitForSingleObject(mutex, INFINITE); });
    abandoned.join();
    check(!reader.read(wire), "reader rejects abandoned snapshot");
    check(!reader.read(wire), "reader keeps abandoned sample quarantined until next publication");
    auto counter = s.sample_counter;
    emit(SCS_TELEMETRY_EVENT_frame_start); a.value_fvector.x = std::numeric_limits<float>::quiet_NaN();
    value(SCS_TELEMETRY_TRUCK_CHANNEL_local_linear_acceleration, &a); emit(SCS_TELEMETRY_EVENT_frame_end);
    s = read(); check(s.active == 0 && std::isfinite(s.sway), "invalid acceleration deactivates frame");
    check(reader.read(wire) && wire.sample_counter == s.sample_counter, "reader recovers after a fresh publication");
    check(s.sample_counter > counter, "sample counter advances");
    emit(SCS_TELEMETRY_EVENT_frame_start); value(SCS_TELEMETRY_TRUCK_CHANNEL_local_linear_acceleration, nullptr);
    emit(SCS_TELEMETRY_EVENT_frame_end); check(read().active == 0, "unavailable channel deactivates frame");
    emit(SCS_TELEMETRY_EVENT_paused); check(read().active == 0, "pause publishes inactive");
    shutdown(); check(read().active == 0, "shutdown leaves inactive snapshot");
    check(duplicate_init(SCS_TELEMETRY_VERSION_1_01, &params) == SCS_RESULT_ok,
          "duplicate can acquire producer ownership after original shutdown");
    duplicate_shutdown(); FreeLibrary(duplicate);
    check(DeleteFileA(duplicate_path.c_str()) != FALSE, "remove duplicate test DLL");
    UnmapViewOfFile(memory); CloseHandle(mapping); CloseHandle(mutex); reader.close();
    channels.clear(); events.clear(); params.common.game_id = "ats";
    check(init(SCS_TELEMETRY_VERSION_1_00, &params) == SCS_RESULT_ok, "ATS and SDK 1.00 supported");
    HANDLE ats = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Local\\OBSRacingMotionATS_v1");
    check(ats != nullptr, "ATS distinct mapping"); CloseHandle(ats); shutdown(); FreeLibrary(dll);
    std::puts("PASS: truck DLL ABI, callbacks, units, invalid input, pause, shutdown, ETS2/ATS");
}
