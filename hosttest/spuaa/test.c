/* Host test of the SPU anti-aliasing kernel (ps3/spu/aa_kernel.h): vector code vs a
 * scalar reference of the same maths, on a synthetic picture with hard edges; writes
 * before/after PPMs to look at. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "aa_kernel.h"

static float G(uint32_t p) { return (float)((p >> 8) & 255); }
static uint32_t ref_px(const uint32_t *img, int w, int h, int x, int y)
{
#define P(dx, dy) img[(y + (dy) < 0 ? 0 : y + (dy) >= h ? h - 1 : y + (dy)) * w + (x + (dx) < 0 ? 0 : x + (dx) >= w ? w - 1 : x + (dx))]
    uint32_t pM = P(0, 0), pN = P(0, -1), pS = P(0, 1), pW = P(-1, 0), pE = P(1, 0), nb;
    float M = G(pM), N = G(pN), S = G(pS), W = G(pW), E = G(pE);
    float NW = G(P(-1, -1)), NE = G(P(1, -1)), SW = G(P(-1, 1)), SE = G(P(1, 1));
    float hi = fmaxf(fmaxf(fmaxf(N, S), fmaxf(W, E)), M), lo = fminf(fminf(fminf(N, S), fminf(W, E)), M);
    float range = hi - lo, eh, ev, sub, wgt, out[3];
    int c;
    if (!(range >= fmaxf(AA_MIN, hi * AA_REL))) return pM;
    eh = fabsf(NW + SW - 2.0f * W) + 2.0f * fabsf(N + S - 2.0f * M) + fabsf(NE + SE - 2.0f * E);
    ev = fabsf(NW + NE - 2.0f * N) + 2.0f * fabsf(W + E - 2.0f * M) + fabsf(SW + SE - 2.0f * S);
    if (eh >= ev) nb = fabsf(N - M) >= fabsf(S - M) ? pN : pS;
    else nb = fabsf(W - M) >= fabsf(E - M) ? pW : pE;
    sub = fabsf((2.0f * (N + S + W + E) + NW + NE + SW + SE) * (1.0f / 12.0f) - M) / fmaxf(range, 1.0f);
    sub = fminf(sub, 1.0f);
    sub = (3.0f - 2.0f * sub) * sub * sub;
    wgt = fmaxf(sub * sub * 0.75f, AA_EDGE);
    for (c = 0; c < 3; c++) {
        float a = (float)((pM >> (16 - 8 * c)) & 255), b = (float)((nb >> (16 - 8 * c)) & 255);
        out[c] = a + (b - a) * wgt + 0.5f;
    }
    return (pM & 0xff000000u) | ((uint32_t)(int)out[0] << 16) | ((uint32_t)(int)out[1] << 8) | (uint32_t)(int)out[2];
#undef P
}

static void ppm(const char *n, const uint32_t *img, int w, int h)
{
    FILE *f = fopen(n, "wb"); int i;
    fprintf(f, "P6 %d %d 255\n", w, h);
    for (i = 0; i < w * h; i++) { unsigned char c[3] = { img[i] >> 16, img[i] >> 8, img[i] }; fwrite(c, 1, 3, f); }
    fclose(f);
}

int main(void)
{
    enum { W = 320, H = 200, NQ = W / 4 };
    static uint32_t img[W * H], out[W * H], ref[W * H];
    static aa_vu rows[3][NQ + 2];
    int x, y, bad = 0;
    /* hard-edged shapes: a rotated square, circles, thin lines, a gradient */
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            uint32_t c = 0xff000000u | (uint32_t)((x * 255 / W) << 16) | 0x2040;
            float u = (x - 80) * 0.8f + (y - 100) * 0.6f, v = -(x - 80) * 0.6f + (y - 100) * 0.8f;
            if (fabsf(u) < 50 && fabsf(v) < 50) c = 0xffe0c020u;
            if ((x - 220) * (x - 220) + (y - 90) * (y - 90) < 55 * 55) c = 0xff3080f0u;
            if (abs(3 * x - 2 * y - 300) < 2) c = 0xffffffffu;
            img[y * W + x] = c;
        }
    for (y = 0; y < H; y++) {
        int k;
        for (k = 0; k < 3; k++) {
            int yy = y + k - 1; yy = yy < 0 ? 0 : yy >= H ? H - 1 : yy;
            memcpy((uint32_t *)rows[k] + 4, img + yy * W, W * 4);
            aa_pad((uint32_t *)rows[k], NQ);
        }
        aa_row(rows[0], rows[1], rows[2], (aa_vu *)(out + y * W), NQ);
        for (x = 0; x < W; x++) {
            ref[y * W + x] = ref_px(img, W, H, x, y);
            if (ref[y * W + x] != out[y * W + x] && bad++ < 5)
                printf("x %d y %d: vector %08x scalar %08x\n", x, y, out[y * W + x], ref[y * W + x]);
        }
    }
    ppm("aa_in.ppm", img, W, H); ppm("aa_out.ppm", out, W, H);
    printf("%d pixels differ between vector and scalar\n", bad);
    return bad != 0;
}
