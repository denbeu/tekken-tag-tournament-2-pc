#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
extern "C" void TTT2CameraObservedPpcStore(uint32_t guestAddr,uint32_t raw,
                                           const char* source,uint32_t line);
// Synthetic fake guest stores: exactly the original big-endian 32-bit bit pattern.
int main() {
    alignas(8) uint8_t fakeGuest[48]={};
    constexpr uint32_t hits[]={0x40094094u,0x40740107u,0x40031518u};
    for (unsigned cycle=0;cycle<120;++cycle) {
        const uint32_t val=hits[cycle%3];
        const uint32_t address=(cycle%10)*4;
        *reinterpret_cast<uint32_t*>(fakeGuest+address)=_byteswap_ulong(val);
        TTT2CameraObservedPpcStore(address,val,__FILE__,__LINE__);
    }
    if (_byteswap_ulong(*reinterpret_cast<uint32_t*>(fakeGuest))!=hits[110%3])
        return 2;
    return 0;
}
