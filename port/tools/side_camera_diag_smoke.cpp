// Standalone smoke harness: does not execute Tekken or access game memory.
#include "../src/runtime/kernel/side_camera_diag.h"
#include <cstdint>
#include <cstdio>
extern "C" void TTT2CameraPpcEntry(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
int main() {
    uint32_t regs[0x8000] = {};
    regs[0x2001] = 0x12340000u;
    regs[0x2002] = 0x00001000u;
    regs[0x2205] = 0x00000001u;
    regs[0x2208] = 0x00000000u;
    ttt2_side_camera_diag::OnConstantWrite(0x4020, 0x3F800000u, 0xA100, 0xA104, 0);
    ttt2_side_camera_diag::OnSwap();
    for (int frame=0;frame<4;++frame) {
        for (uint32_t i=0;i<60;++i)
            TTT2CameraPpcEntry(0x82550C58u, 0x81000000u+i, 1u, 2u, 3u, 0x82551870u);
        for (uint32_t i=0;i<60;++i)
            TTT2CameraPpcEntry(0x82545458u, 0x82000000u+i, 4u, 5u, 6u, 0x82545D48u);
        for (uint32_t i=0;i<280;++i) {
            auto reg = 0x4020u + (i % 16u);
            ttt2_side_camera_diag::OnConstantWrite(reg, 0x3F800000u + i,
                0xA100u + i * 4, 0xA104u + i * 4, 0x12Fu);
        }
        for (uint32_t i=0;i<150;++i) {
            regs[0x4020] = 0x3F800000u+i;
            if (ttt2_side_camera_diag::WantsDrawHash())
                ttt2_side_camera_diag::OnDraw(regs, i, 0x82211111u, 64, 12345,
                    0x82222222u, 96, 67890);
            else ttt2_side_camera_diag::OnSkippedDraw();
        }
        ttt2_side_camera_diag::OnSwap();
    }
    std::puts("side-camera smoke completed");
    return 0;
}
