#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace racing::truck {
inline constexpr uint32_t kMagic = 0x4d52544f;
inline constexpr uint32_t kVersion = 1;
struct Snapshot {
    uint32_t magic, version, size, active;
    uint64_t sample_counter, timestamp_ms;
    float surge, sway, heave, pitch, roll, yaw, speed, rpm;
    int32_t gear;
    uint32_t reserved;
};
static_assert(std::is_trivially_copyable_v<Snapshot>);
static_assert(sizeof(Snapshot) == 72 && offsetof(Snapshot, surge) == 32 && offsetof(Snapshot, gear) == 64);
inline constexpr size_t kMappingSize = sizeof(Snapshot);
inline const wchar_t* mapping_name(bool ats) noexcept {
    return ats ? L"Local\\OBSRacingMotionATS_v1" : L"Local\\OBSRacingMotionETS2_v1";
}
inline const wchar_t* mutex_name(bool ats) noexcept {
    return ats ? L"Local\\OBSRacingMotionATS_v1_Mutex" : L"Local\\OBSRacingMotionETS2_v1_Mutex";
}

// Single-thread owned. Kernel mutex serializes memcpy in both processes; there
// are no concurrent non-atomic payload accesses. Neither side waits for a lock.
class Reader {
public:
    explicit Reader(bool ats = false) noexcept : ats_(ats) {}
    ~Reader() { close(); }
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    void close() noexcept {
        if (view_) UnmapViewOfFile(view_);
        if (mapping_) CloseHandle(mapping_);
        if (mutex_) CloseHandle(mutex_);
        view_ = nullptr; mapping_ = nullptr; mutex_ = nullptr;
        quarantined_ = false;
    }
    bool read(Snapshot& output) noexcept {
        if (!view_) {
            mutex_ = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, mutex_name(ats_));
            if (mutex_) mapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, mapping_name(ats_));
            if (mapping_) view_ = static_cast<const Snapshot*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, kMappingSize));
            if (!view_) { close(); return false; }
        }
        const DWORD result = WaitForSingleObject(mutex_, 0);
        if (result == WAIT_ABANDONED) {
            // A producer may have died mid-copy. Discard this snapshot.
            abandoned_counter_ = view_->sample_counter;
            quarantined_ = true;
            ReleaseMutex(mutex_); return false;
        }
        if (result != WAIT_OBJECT_0) {
            if (result == WAIT_FAILED) close();
            return false;
        }
        if (quarantined_ && view_->sample_counter == abandoned_counter_) {
            ReleaseMutex(mutex_); return false;
        }
        quarantined_ = false;
        std::memcpy(&output, view_, sizeof(output));
        ReleaseMutex(mutex_);
        return true;
    }
private:
    bool ats_;
    HANDLE mapping_ = nullptr, mutex_ = nullptr;
    const Snapshot* view_ = nullptr;
    uint64_t abandoned_counter_ = 0;
    bool quarantined_ = false;
};
} // namespace racing::truck
