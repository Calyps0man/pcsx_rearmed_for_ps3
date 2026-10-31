/* Temporary CFW storage permissions. No flash writes and no payload install.
 * The four LV1 edits and LV2 storage flag follow Iris Manager/IRISMAN.
 * Unlike a blind firmware table, validate ALL sites before writing any of them.
 * Hardware access lives in disc.c so this transaction can be host-tested. */
#ifndef PS3_DISC_ACCESS_H
#define PS3_DISC_ACCESS_H
#include <stdint.h>
#include <string.h>

typedef uint64_t (*disc_peek_fn)(int level, uint64_t address);
typedef void (*disc_poke_fn)(int level, uint64_t address, uint64_t value);
typedef struct {
    uint64_t address, original, patched;
    int level, changed;
} disc_patch_site;
typedef struct { disc_patch_site site[5]; } disc_access;

/* Return failure rather than overwriting a value another component changed. */
static int disc_access_restore(disc_access *a, disc_peek_fn peek, disc_poke_fn poke)
{
    int i, failed = 0;
    for (i = 4; i >= 0; --i) {
        disc_patch_site *s = &a->site[i];
        if (!s->changed) continue;
        if (peek(s->level, s->address) != s->patched) { failed = 1; continue; }
        poke(s->level, s->address, s->original);
        if (peek(s->level, s->address) != s->original) failed = 1;
        else s->changed = 0;
    }
    return failed ? -1 : 0;
}

static int disc_access_enable(disc_access *a, disc_peek_fn peek, disc_poke_fn poke)
{
    static const uint64_t hvaddr[4] = { 0x16fa60, 0x16fa84, 0x16fafc, 0x16fb04 };
    static const uint64_t value[4] = {
        UINT64_C(0x7f83e37860000000), UINT64_C(0x7f85e37838600001),
        UINT64_C(0x7f84e3783be00001), UINT64_C(0x9be1007038600000)
    };
    const uint64_t k = UINT64_C(0x8000000000000000);
    uint64_t flag = 0, marker = UINT64_C(0x4345580000000000); /* CEX */
    uint64_t toc = peek(2, k + 0x3000);
    int i;
    memset(a, 0, sizeof *a);
    /* Kernel layout markers, not the advertised/spoofed firmware version.
     * Retail 4.75-4.78 / 4.81+ share the +0x10 layout; 4.80/4.90 use -0x10.
     * DEX 4.81+ (including PEX's DEX kernel) has a different flag location. */
    if (toc == k + 0x34fbb0 && peek(2, k + 0x2ed818) == marker) flag = k + 0x2ecab0;
    else if (toc == k + 0x34fba0 && peek(2, k + 0x2ed808) == marker) flag = k + 0x2ecaa0;
    else if (toc == k + 0x3759c0 && peek(2, k + 0x30f3b0) == UINT64_C(0x4445580000000000)) flag = k + 0x30e648;
    if (!flag) return -1;
    for (i = 0; i < 4; ++i) {
        uint64_t old = peek(1, hvaddr[i]);
        uint32_t ins = (uint32_t)old;
        int valid = old == value[i];
        /* The branch displacement can differ; opcode/link bit must match. */
        if ((old >> 32) == (value[i] >> 32)) {
            if (i == 0) valid |= ins == 0xf8010098;
            if (i == 1 || i == 3) valid |= (ins & 0xfc000003U) == 0x48000001U;
            if (i == 2) valid |= ins == 0x38a10070;
        }
        if (!valid) return -2;
        a->site[i] = (disc_patch_site){ hvaddr[i], old, value[i], 1, 0 };
    }
    {
        uint64_t old = peek(2, flag);
        /* Full37 wrongly guessed a three-value whitelist for this word.
         * IRISMAN writes the upper 32 bits at this layout-specific site,
         * preserving its lower half, without that assumption. Establish the
         * address from BOTH kernel TOC and CEX/DEX marker instead. */
        if (old == UINT64_MAX) return -3;
        a->site[4] = (disc_patch_site){ flag, old,
            (UINT64_C(0x40000000) << 32) | (uint32_t)old, 2, 0 };
    }
    for (i = 0; i < 5; ++i) {
        disc_patch_site *s = &a->site[i];
        /* Recheck immediately before writing and roll back on any failure. */
        if (peek(s->level, s->address) != s->original) goto failed;
        if (s->original == s->patched) continue;
        s->changed = 1;
        poke(s->level, s->address, s->patched);
        if (peek(s->level, s->address) != s->patched) {
            /* A rejected/no-op poke needs no restore. */
            if (peek(s->level, s->address) == s->original) s->changed = 0;
            goto failed;
        }
    }
    return 0;
failed:
    disc_access_restore(a, peek, poke);
    return -4;
}
#endif
