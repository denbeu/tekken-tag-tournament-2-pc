// Optional, bounded read-only PPC function-entry sampling.
// Explicitly selected recompiled guest functions only; never modifies PPCContext or guest data.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <array>

namespace {
constexpr uint32_t kTargets[] = {
    0x82550C58, 0x825511B8, 0x82551870, 0x82545458,
    0x82545D48, 0x8254A200, 0x82548CB0, 0x8254E9D8,
    0x8254C9B8, 0x82556598, 0x8254FD90
};
constexpr size_t kNumTargets = sizeof(kTargets) / sizeof(kTargets[0]);
constexpr unsigned kPerFunction = 32;

struct State {
    std::mutex mutex;
    bool initialized = false;
    bool enabled = false;
    bool failed = false;
    HANDLE file = INVALID_HANDLE_VALUE;
    char trigger[1024] = {};
    char out[1024] = {};
    std::array<unsigned, kNumTargets> counts = {};
    ~State() {
        if (file != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(file);
            CloseHandle(file);
        }
    }
};
State g;

bool ReadEnv(const char* key, char (&out)[1024]) {
    const DWORD n = GetEnvironmentVariableA(key, out, DWORD(sizeof(out)));
    return n > 0 && n < sizeof(out);
}
} // namespace

extern "C" void TTT2CameraPpcEntry(uint32_t func, uint32_t r3,
                                     uint32_t r4, uint32_t r5,
                                     uint32_t r6, uint32_t lr) {
    std::lock_guard<std::mutex> lock(g.mutex);
    if (!g.initialized) {
        g.initialized = true;
        char enabled[8] = {};
        if (GetEnvironmentVariableA("TTT2_SIDE_CAMERA_PPC", enabled, DWORD(sizeof(enabled))) != 1 ||
            enabled[0] != '1' ||
            !ReadEnv("TTT2_SIDE_CAMERA_TRIGGER", g.trigger) ||
            !ReadEnv("TTT2_SIDE_CAMERA_PPC_OUT", g.out)) return;
        g.enabled = true;
    }
    if (!g.enabled || g.failed) return;
    unsigned index = 0;
    while (index < kNumTargets && kTargets[index] != func) ++index;
    if (index >= kNumTargets || g.counts[index] >= kPerFunction) return;
    if (g.file == INVALID_HANDLE_VALUE) {
        const DWORD attr = GetFileAttributesA(g.trigger);
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) return;
        g.file = CreateFileA(g.out, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g.file == INVALID_HANDLE_VALUE) {
            std::fprintf(stderr, "[camera-ppc] output create refused, error %lu\n", GetLastError());
            g.failed = true;
            return;
        }
    }
    char line[260];
    const int n = std::snprintf(line, sizeof(line),
       "{\"type\":\"ppc\",\"tick_ms\":%llu,\"function\":\"0x%08X\",\"r3\":\"%08X\",\"r4\":\"%08X\",\"r5\":\"%08X\",\"r6\":\"%08X\",\"lr\":\"%08X\"}\n",
       static_cast<unsigned long long>(GetTickCount64()), func, r3, r4, r5, r6, lr);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(line)) {
        g.failed = true;
        return;
    }
    DWORD written = 0;
    if (!WriteFile(g.file, line, DWORD(n), &written, nullptr) || written != DWORD(n)) {
        std::fprintf(stderr, "[camera-ppc] output write failed\n");
        g.failed = true;
        return;
    }
    ++g.counts[index];
}
