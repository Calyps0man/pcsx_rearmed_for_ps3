/* Host test of the SPU FMV decoder: ps3/src/spu_mdec.c (dispatcher) with
 * ps3/spu/mdec_spu.c (SPU program) run directly, DMA rules checked. Random
 * streams are read back in random DMA sizes and compared with the core's decoder. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
/* PS1 data is little-endian: swap on the big-endian PPU/SPU, not on a PC test host */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define MDEC_SWAP16(x) ((unsigned short)((((x) & 0xff) << 8) | (((x) >> 8) & 0xff)))
#else
#define MDEC_SWAP16(x) ((unsigned short)(x))
#endif
#include "mdec_decode.h"
#include "aa_kernel.h"
int spu_aa_start(uint64_t in_ea, uint64_t out_ea, uint64_t label_ea, uint32_t label_value, unsigned w, unsigned h, unsigned pitch);
int spu_aa_wait(unsigned max_us);

/* the SPU anti-aliasing job path: 5 bands, DMA rules, label wait, against aa_row
   over the whole picture */
static int aa_test(void)
{
    enum { W = 1440, H = 203, P = 1920 * 4 };
    static uint32_t in[H * P / 4] __attribute__((aligned(128))), out[H * P / 4] __attribute__((aligned(128)));
    static uint32_t ref[W];
    static aa_vu rows[3][W / 4 + 2];
    static uint32_t label[4] __attribute__((aligned(16)));
    int x, y, bad = 0, r;
    unsigned s = 7;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            s = s * 1103515245u + 12345u;
            in[y * P / 4 + x] = ((x / 7 + y / 5) & 1) ? 0xff204080u : 0xffe0c030u ^ ((s >> 20) & 0x0f0f0f);
        }
    label[0] = 0x12341234u;
    if (!spu_aa_start((uintptr_t)in, (uintptr_t)out, (uintptr_t)label, 0x12341234u, W, H, P)) { printf("aa: start failed\n"); return 1; }
    r = spu_aa_wait(100000);
    if (r != 1) { printf("aa: wait returned %d\n", r); return 1; }
    for (y = 0; y < H; y++) {
        int k;
        for (k = 0; k < 3; k++) {
            int yy = y + k - 1; yy = yy < 0 ? 0 : yy >= H ? H - 1 : yy;
            memcpy((uint32_t *)rows[k] + 4, in + yy * P / 4, W * 4);
            aa_pad((uint32_t *)rows[k], W / 4);
        }
        aa_row(rows[0], rows[1], rows[2], (aa_vu *)ref, W / 4);
        if (memcmp(ref, out + y * P / 4, W * 4) && bad++ < 3) printf("aa: row %d differs\n", y);
    }
    /* a label that never comes: the SPU gives up, the PPU hears so */
    label[0] = 1;
    spu_aa_start((uintptr_t)in, (uintptr_t)out, (uintptr_t)label, 2, W, H, P);
    r = spu_aa_wait(100000);
    if (r != -1) { printf("aa: missing label: wait returned %d\n", r); bad++; }
    printf("aa: %d rows wrong\n", bad);
    return bad != 0;
}

void (*mdec_stream_hook)(const unsigned short *, const unsigned short *, int, int, int, const int *, const int *);
int (*mdec_decode_hook)(const unsigned short **, unsigned char *, int, int, int, int, const int *, const int *);
int spu_mdec_init(int nspu);
void spu_mdec_enable(int on);
void spu_mdec_stats(unsigned *streams, unsigned *mbs, unsigned *misses, double *wait_ms);

static unsigned s = 99;
static unsigned r(void) { s = s * 1664525u + 1013904223u; return s >> 8; }

static size_t gen(unsigned short *w, int nmb)
{
    size_t p = 0;
    for (int m = 0; m < nmb; m++)
        for (int b = 0; b < 6; b++) {
            int k = 0, len = (int)(r() % 40);
            w[p++] = MDEC_SWAP16(((r() % 64) << 10) | (r() & 0x3ff));
            while (len--) {
                int run = (int)(r() % 8);
                if (k + run + 1 > 63) break;
                k += run + 1;
                w[p++] = MDEC_SWAP16((unsigned)(run << 10) | (r() & 0x3ff));
            }
            if (r() % 10 == 0) w[p++] = MDEC_SWAP16((unsigned)(63 << 10) | 7); else w[p++] = MDEC_SWAP16(0xfe00);
        }
    w[p++] = MDEC_SWAP16(0xfe00);
    return p;
}

int main(int argc, char **argv)
{
    int iters = argc > 1 ? atoi(argv[1]) : 300, it, bad = 0;
    static int iqy[64], iquv[64];
    unsigned short *ram = memalign(128, 2 << 20);
    unsigned char *ref = malloc(1600 * 768), *got = malloc(1600 * 768);
    if (!spu_mdec_init(5)) { printf("init/self-test failed\n"); return 1; }
    if (aa_test()) bad++;
    spu_mdec_enable(1);
    for (it = 0; it < iters; it++) {
        int nmb = 1 + (int)(r() % 1300), rgb24 = r() & 1, stp = (r() & 1) ? 0x8000 : 0, bw = (r() % 8) == 0;
        int bs = rgb24 ? 768 : 512, done = 0, i, blk[384];
        unsigned short *st = ram + 2 * (r() % 4096);             /* word-aligned like DMA addresses */
        size_t len;
        const unsigned short *rl;
        for (i = 0; i < 64; i++) { iqy[i] = 1000 + (int)(r() % 50000); iquv[i] = 1000 + (int)(r() % 50000); }
        len = gen(st, nmb);
        rl = st;
        for (i = 0; i < nmb; i++) {
            rl = rl2blk(blk, rl, iqy, iquv);
            if (rgb24) yuv2rgb24(blk, ref + i * bs, bw); else yuv2rgb15(blk, (unsigned short *)(ref + i * bs), stp, bw);
        }
        memset(got, 0xee, (size_t)nmb * bs);
        mdec_stream_hook(st, st + len, rgb24, stp, bw, iqy, iquv);
        rl = st;
        while (done < nmb) {          /* like psxDma1: hook first, the core decodes what it leaves */
            int n = 1 + (int)(r() % 40), k;
            if (n > nmb - done) n = nmb - done;
            k = mdec_decode_hook(&rl, got + done * bs, n, rgb24, stp, bw, iqy, iquv);
            for (i = k; i < n; i++) {
                rl = rl2blk(blk, rl, iqy, iquv);
                if (rgb24) yuv2rgb24(blk, got + (done + i) * bs, bw); else yuv2rgb15(blk, (unsigned short *)(got + (done + i) * bs), stp, bw);
            }
            done += n;
        }
        if (memcmp(ref, got, (size_t)nmb * bs) || rl != st + len - 1) {
            printf("iteration %d (nmb %d rgb24 %d): mismatch\n", it, nmb, rgb24); bad++;
        }
        if (r() % 5 == 0) mdec_stream_hook(0, 0, 0, 0, 0, 0, 0);    /* reset in between */
    }
    {
        unsigned st_, mbs, miss; double w;
        spu_mdec_stats(&st_, &mbs, &miss, &w);
        printf("%d iterations, %d bad; streams %u, macroblocks from the SPUs %u, misses %u\n", iters, bad, st_, mbs, miss);
    }
    return bad != 0;
}
