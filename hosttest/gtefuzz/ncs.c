#include <stdio.h>
#include <string.h>
#include "psxcommon.h"
#include "r3000a.h"
#include "gte.h"
psxRegisters psxRegs;
int main(void)
{
    psxCP2Regs *r = (psxCP2Regs *)&psxRegs.CP2D;
    int k;
    for (k = 0; k < 3; k++) {
        memset(r, 0, sizeof *r);
        /* light matrix = identity (4096), light colour matrix LR1..LB3 = 1..9 */
        CTC2(r, 0x1000, 8); CTC2(r, 0, 9); CTC2(r, 0x1000, 10); CTC2(r, 0, 11); CTC2(r, 0x1000, 12);
        CTC2(r, 1 | 2 << 16, 16); CTC2(r, 3 | 4 << 16, 17); CTC2(r, 5 | 6 << 16, 18); CTC2(r, 7 | 8 << 16, 19); CTC2(r, 9, 20);
        MTC2(r, (k == 0 ? 0x1000 : 0) | (k == 1 ? 0x1000 : 0) << 16, 0);
        MTC2(r, k == 2 ? 0x1000 : 0, 1);
        gteDispatch(r, 0x4a08001e);   /* NCS sf=1 */
        printf("V0 axis %d: L*V -> IR %d %d %d  MAC %d %d %d\n", k, (s16)MFC2(r, 9), (s16)MFC2(r, 10), (s16)MFC2(r, 11),
               (int)MFC2(r, 25), (int)MFC2(r, 26), (int)MFC2(r, 27));
    }
    return 0;
}
