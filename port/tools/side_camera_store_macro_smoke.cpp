#include "ppc_context.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
int main() {
    alignas(32) unsigned char mem[128]={};
    unsigned char* base=mem;
    uint32_t offset=0;
    uint32_t raw=0x40094094u;
    // A generated guest scalar store must evaluate each expression only once.
    PPC_STORE_U32((offset+=4), (raw++));
    if (offset!=4 || raw!=0x40094095u || PPC_LOAD_U32(4)!=0x40094094u) {
        std::fputs("Scalar guest-store macro semantics changed\n",stderr);
        return 2;
    }
    // Unmatched stores must not be passed to the writer observer.
    PPC_STORE_U32(8,0x12345678u);
    if (PPC_LOAD_U32(8)!=0x12345678u) return 3;
    return 0;
}
