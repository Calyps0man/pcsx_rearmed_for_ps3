/* Edge anti-aliasing for the SPUs (FXAA-style, on whole pixels so it vectorises
 * without gathers). Per pixel, from the green channel of its 3x3 neighbourhood:
 *  - local contrast below max(AA_MIN, AA_REL * brightest) -> unchanged;
 *  - else the edge direction from FXAA's second-derivative test (horizontal edge:
 *    blend with the pixel above or below, vertical: left or right), towards the
 *    side with the larger step;
 *  - blend amount = max(AA_EDGE, FXAA's sub-pixel term from the 3x3 low-pass).
 * Written with GCC vector extensions: the same code runs on the SPU (spu-gcc) and
 * in the host test, which checks it against aa_ref() below.
 *
 * Pixels are 32-bit values 0x??RRGGBB (as the RSX writes X8R8G8B8, read as
 * big-endian words; the test host uses native words). Rows are passed with one
 * padding quad (4 pixels) on each side, filled by the caller. */
#ifndef AA_KERNEL_H
#define AA_KERNEL_H
#include <stdint.h>

#define AA_MIN  10.0f      /* contrast floor (0..255 units) */
#define AA_REL  0.125f     /* contrast relative to the brightest neighbour */
#define AA_EDGE 0.35f      /* blend across a detected edge */

typedef float    aa_vf __attribute__((vector_size(16)));
typedef int32_t  aa_vi __attribute__((vector_size(16)));
typedef uint32_t aa_vu __attribute__((vector_size(16)));

static inline aa_vf aa_g(aa_vu p) { return __builtin_convertvector((aa_vi)((p >> 8) & 255u), aa_vf); }
static inline aa_vf aa_sel(aa_vi m, aa_vf a, aa_vf b) { return (aa_vf)((m & (aa_vi)a) | (~m & (aa_vi)b)); }
static inline aa_vf aa_max(aa_vf a, aa_vf b) { return aa_sel(a > b, a, b); }
static inline aa_vf aa_min(aa_vf a, aa_vf b) { return aa_sel(a < b, a, b); }
static inline aa_vf aa_abs(aa_vf a) { return aa_max(a, -a); }
static inline aa_vf aa_splat(float f) { return (aa_vf){ f, f, f, f }; }

/* the quad at index q of a padded row, and its neighbours one pixel left / right */
static inline aa_vu aa_l(const aa_vu *r, int q) { return __builtin_shuffle(r[q - 1], r[q], (aa_vu){ 3, 4, 5, 6 }); }
static inline aa_vu aa_r(const aa_vu *r, int q) { return __builtin_shuffle(r[q], r[q + 1], (aa_vu){ 1, 2, 3, 4 }); }

static inline aa_vf aa_blend_ch(aa_vu m, aa_vu n, int shift, aa_vf w)
{
    aa_vf a = __builtin_convertvector((aa_vi)((m >> shift) & 255u), aa_vf);
    aa_vf b = __builtin_convertvector((aa_vi)((n >> shift) & 255u), aa_vf);
    return a + (b - a) * w + 0.5f;
}

/* one output row: up/mid/dn are padded input rows (index 1 = first real quad),
   out gets nq quads */
static void aa_row(const aa_vu *up, const aa_vu *mid, const aa_vu *dn, aa_vu *out, int nq)
{
    int q;
    for (q = 1; q <= nq; q++) {
        aa_vu pM = mid[q], pN = up[q], pS = dn[q], pW = aa_l(mid, q), pE = aa_r(mid, q);
        aa_vf M = aa_g(pM), N = aa_g(pN), S = aa_g(pS), W = aa_g(pW), E = aa_g(pE);
        aa_vf NW = aa_g(aa_l(up, q)), NE = aa_g(aa_r(up, q)), SW = aa_g(aa_l(dn, q)), SE = aa_g(aa_r(dn, q));
        aa_vf hi = aa_max(aa_max(aa_max(N, S), aa_max(W, E)), M);
        aa_vf lo = aa_min(aa_min(aa_min(N, S), aa_min(W, E)), M);
        aa_vf range = hi - lo;
        aa_vi edge = range >= aa_max(aa_splat(AA_MIN), hi * AA_REL);
        aa_vf eh, ev, sub, w, rr, gg, bb;
        aa_vi horz, up_side, left_side;
        aa_vu nb, o;
        if (!(edge[0] | edge[1] | edge[2] | edge[3])) { out[q - 1] = pM; continue; }
        eh = aa_abs(NW + SW - 2.0f * W) + 2.0f * aa_abs(N + S - 2.0f * M) + aa_abs(NE + SE - 2.0f * E);
        ev = aa_abs(NW + NE - 2.0f * N) + 2.0f * aa_abs(W + E - 2.0f * M) + aa_abs(SW + SE - 2.0f * S);
        horz = eh >= ev;                                   /* edge runs horizontally */
        up_side = aa_abs(N - M) >= aa_abs(S - M);
        left_side = aa_abs(W - M) >= aa_abs(E - M);
        nb = (aa_vu)(horz & (aa_vi)((aa_vu)(up_side & (aa_vi)pN) | (aa_vu)(~up_side & (aa_vi)pS)))
           | (aa_vu)(~horz & (aa_vi)((aa_vu)(left_side & (aa_vi)pW) | (aa_vu)(~left_side & (aa_vi)pE)));
        /* FXAA sub-pixel term: how far the pixel is from its low-passed neighbourhood */
        sub = aa_abs((2.0f * (N + S + W + E) + NW + NE + SW + SE) * (1.0f / 12.0f) - M) / aa_max(range, aa_splat(1.0f));
        sub = aa_min(sub, aa_splat(1.0f));
        sub = (3.0f - 2.0f * sub) * sub * sub;
        w = aa_max(sub * sub * 0.75f, aa_splat(AA_EDGE));
        w = aa_sel(edge, w, aa_splat(0.0f));
        rr = aa_blend_ch(pM, nb, 16, w);
        gg = aa_blend_ch(pM, nb, 8, w);
        bb = aa_blend_ch(pM, nb, 0, w);
        o = (pM & 0xff000000u) | ((aa_vu)__builtin_convertvector(rr, aa_vi) << 16)
          | ((aa_vu)__builtin_convertvector(gg, aa_vi) << 8) | (aa_vu)__builtin_convertvector(bb, aa_vi);
        out[q - 1] = o;
    }
}

/* fills the padding quads of a row (pixels 0..4*nq-1 at r + 4) with its edge pixels */
static inline void aa_pad(uint32_t *r, int nq)
{
    int i;
    for (i = 0; i < 4; i++) { r[i] = r[4]; r[4 + 4 * nq + i] = r[4 + 4 * nq - 1]; }
}

#endif
