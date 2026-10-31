/* Differential GTE fuzz: same random register states and commands on any build;
 * prints a checksum per command plus the full register file every 4096 cases.
 * Run natively (little-endian) and under qemu-ppc64 (big-endian); outputs must match. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "psxcommon.h"
#include "r3000a.h"
#include "gte.h"

psxRegisters psxRegs;
static unsigned long long rs = 88172645463325252ull;
static u32 rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (u32)(rs >> 11); }

int main(void)
{
    static const u8 ops[] = { 0x01, 0x06, 0x0c, 0x10, 0x11, 0x12, 0x13, 0x14, 0x16, 0x1b, 0x1c, 0x1e,
                              0x20, 0x28, 0x29, 0x2a, 0x2d, 0x2e, 0x30, 0x3d, 0x3e, 0x3f };
    unsigned n, i;
    u32 sum = 0;
    for (n = 0; n < 200000; n++) {
        psxCP2Regs *r = (psxCP2Regs *)&psxRegs.CP2D;
        u32 code;
        for (i = 0; i < 32; i++) {
            u32 v = rnd();
            if (rnd() & 1) v &= 0x0fff0fff;          /* plenty of small values too */
            MTC2(r, v, i);
        }
        for (i = 0; i < 31; i++) CTC2(r, (rnd() & 1) ? rnd() : rnd() & 0x00ff0fff, i);
        code = (rnd() & 0x01fffc00) | ops[rnd() % sizeof ops] | 0x4a000000;
        if (getenv("DUMP") && n < 64) { for (i = 0; i < 32; i++) printf("pre d%u %08x\n", i, MFC2(r, i)); }
        gteDispatch(r, code);
        if (getenv("DUMP") && n < 64) { printf("case %u op %08x\n", n, code); for (i = 0; i < 32; i++) printf("d%u %08x c%u %08x\n", i, MFC2(r, i), i, r->CP2C.r[i]); }
        for (i = 0; i < 32; i++) sum = sum * 31 + MFC2(r, i);
        for (i = 0; i < 32; i++) sum = sum * 31 + r->CP2C.r[i];
        if ((n & 4095) == 4095) printf("%u %08x\n", n, (unsigned)sum);
    }
    printf("final %08x\n", (unsigned)sum);
    return 0;
}
