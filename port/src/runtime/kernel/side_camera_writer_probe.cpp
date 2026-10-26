// Experimental read-only candidate store log for TU3 camera-matrix producer tracing.
// Only called for exact value matches in opt-in generated PPC code; no guest writes.
// Output CREATE_NEW; bounded to 256 records; never used by installed game's EXE.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace {
constexpr unsigned kRecordLimit = 256;
struct Trace {
    std::mutex lock;
    bool initialised=false, enabled=false, disabled=false;
    HANDLE out=INVALID_HANDLE_VALUE;
    std::array<char,1024> trigger{};
    std::array<char,1024> path{};
    unsigned count=0, rejected=0;
    ~Trace() { if (out!=INVALID_HANDLE_VALUE) { FlushFileBuffers(out); CloseHandle(out); } }
};
Trace g;
bool GetPath(const char* key,std::array<char,1024>& dst) {
    const DWORD n=GetEnvironmentVariableA(key,dst.data(),DWORD(dst.size()));
    return n>0 && n<dst.size();
}
} // namespace
extern "C" void TTT2CameraObservedPpcStore(uint32_t guestAddr,uint32_t raw,
                                           const char* file,uint32_t line) {
    std::lock_guard<std::mutex> guard(g.lock);
    if (!g.initialised) {
        g.initialised=true;
        char enabled[8]={};
        if (GetEnvironmentVariableA("TTT2_SIDE_CAMERA_WRITER", enabled, DWORD(sizeof(enabled)))!=1
            || enabled[0]!='1'
            || !GetPath("TTT2_SIDE_CAMERA_TRIGGER",g.trigger)
            || !GetPath("TTT2_SIDE_CAMERA_WRITER_OUT",g.path)) {
            g.disabled=true;
            return;
        }
        g.enabled=true;
    }
    if (!g.enabled || g.disabled || g.count>=kRecordLimit) return;
    if (g.out==INVALID_HANDLE_VALUE) {
        DWORD att=GetFileAttributesA(g.trigger.data());
        if (att==INVALID_FILE_ATTRIBUTES || (att & FILE_ATTRIBUTE_DIRECTORY)) return;
        g.out=CreateFileA(g.path.data(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,
                          CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (g.out==INVALID_HANDLE_VALUE) {
            std::fprintf(stderr,"[camera-writer] refusing output path: %s (error %lu)\n",
                         g.path.data(),GetLastError());
            g.disabled=true;
            return;
        }
    }
    const char* basename=file;
    if (const char* p=std::strrchr(file,'\\')) basename=p+1;
    if (const char* p=std::strrchr(basename,'/')) basename=p+1;
    // Names come from compiled source filenames, never user data.
    char buf[370];
    const int n=std::snprintf(buf,sizeof(buf),
       "{\"type\":\"ppc_store_candidate\",\"tick_ms\":%llu,\"guest\":\"%08X\",\"raw\":\"%08X\",\"file\":\"%.70s\",\"line\":%u,\"tid\":%lu}\n",
       static_cast<unsigned long long>(GetTickCount64()),guestAddr,raw,basename,line,GetCurrentThreadId());
    if (n<0 || size_t(n)>=sizeof(buf)) {g.disabled=true;return;}
    DWORD wrote=0;
    if (!WriteFile(g.out,buf,DWORD(n),&wrote,nullptr) || wrote!=DWORD(n)) {
        g.disabled=true;
        return;
    }
    ++g.count;
}
