/* SPU program: the emulator's SPU worker. Two kinds of jobs:
 *  - MDEC: decodes FMV macroblocks. Fetches the job (mdec_job.h), streams the
 *    run-length data in, decodes with the core's own routines
 *    (libpcsxcore/mdec_decode.h, so the output is identical to the PPU's).
 *  - anti-aliasing: a band of rows of the picture the RSX wrote to main memory
 *    (aa_job.h, aa_kernel.h), after waiting for the RSX's label.
 * Runs as a persistent SPU thread: the PPU writes the job to main memory and a
 * word to this thread's inbound mailbox (MDEC job index + 1, or 0x100 + AA job
 * index); the job's sequence number goes to done_ea when its output is in memory.
 * A zero mailbox word ends the thread.
 *
 * Built with -DHOST_TEST, process_job() runs on a PC with memcpy for DMA, and
 * every transfer is checked against the MFC's alignment and size rules. */
#include <stdint.h>
#include <string.h>
#include "mdec_job.h"
#include "aa_job.h"
#include "aa_kernel.h"

/* PS1 data is little-endian: swap on the big-endian PPU/SPU, not on a PC test host */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define MDEC_SWAP16(x) ((unsigned short)((((x) & 0xff) << 8) | (((x) >> 8) & 0xff)))
#else
#define MDEC_SWAP16(x) ((unsigned short)(x))
#endif
#include "mdec_decode.h"

#ifdef HOST_TEST
#include <assert.h>
#include <stdio.h>
static void dma_check(const void *ls, uint64_t ea, uint32_t size)
{
    assert(size > 0 && size <= 16384);
    assert(size % 16 == 0);
    assert(((uintptr_t)ls & 15) == 0 && (ea & 15) == 0);
}
#define DMA_GET(ls, ea, size, tag) (dma_check(ls, ea, size), memcpy((ls), (void *)(uintptr_t)(ea), (size)))
#define DMA_PUT(ls, ea, size, tag) (dma_check(ls, ea, size), memcpy((void *)(uintptr_t)(ea), (ls), (size)))
#define DMA_WAIT(mask) ((void)0)
#else
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>
#define DMA_GET(ls, ea, size, tag) mfc_get((ls), (ea), (size), (tag), 0, 0)
#define DMA_PUT(ls, ea, size, tag) mfc_put((ls), (ea), (size), (tag), 0, 0)
#define DMA_WAIT(mask) do { mfc_write_tag_mask(mask); spu_mfcstat(MFC_TAG_UPDATE_ALL); } while (0)
#endif

#define TAG_IN   1
#define TAG_OUT0 2            /* + output buffer index */
#define TAG_DONE 4

#define RLBUF  (32 * 1024)    /* run-length data window */
#define OUTMB  16             /* macroblocks per output buffer (12 KiB at 24 bpp, under the 16 KiB DMA limit) */
#define AA_Q   (AA_MAX_W / 4) /* quads per row */
#define AA_CH  4              /* output rows per step */

/* one work area for either job kind (jobs run one at a time) */
static union {
    struct { unsigned char rl[RLBUF]; unsigned char out[2][OUTMB * 768]; } m;
    struct { aa_vu in[AA_CH + 2][AA_Q + 2]; aa_vu out[2][AA_CH][AA_Q]; } a;
} work __attribute__((aligned(128)));
#define rlbuf  work.m.rl
#define outbuf work.m.out

static mdec_job job __attribute__((aligned(128)));
static aa_job ajob __attribute__((aligned(128)));
static int blk[6 * 64];
static uint32_t done_word[4] __attribute__((aligned(16)));

/* fetches up to `want` bytes (multiple of 16) from ea into ls, 16 KiB at a time */
static void fetch(unsigned char *ls, uint64_t ea, uint32_t want)
{
    while (want) {
        uint32_t n = want > 16384 ? 16384 : want;
        DMA_GET(ls, ea, n, TAG_IN);
        ls += n; ea += n; want -= n;
    }
    DMA_WAIT(1 << TAG_IN);
}

static void process_job(void)
{
    uint64_t ea = job.rl_ea;          /* next byte to fetch */
    uint32_t left = job.rl_size;      /* bytes still to fetch, multiple of 16 */
    uint32_t have, pos = job.rl_skip; /* valid bytes in rlbuf, read position */
    uint32_t bsize = (job.flags & MDEC_JOB_RGB24) ? 768 : 512;
    int stp = (job.flags & MDEC_JOB_STP) ? 0x8000 : 0, bw = (job.flags & MDEC_JOB_BW) != 0;
    uint32_t i, nb = 0, cur = 0, out_off = 0;
    uint32_t pending = 0;             /* output buffers with a put in flight (bit mask) */

    have = left > RLBUF ? RLBUF : left;
    fetch(rlbuf, ea, have);
    ea += have; left -= have;

    for (i = 0; i < job.count; i++) {
        const unsigned short *p, *e;
        if (have - pos < MDEC_MAX_MB_BYTES && left) {
            /* slide the unread tail (from its 16-byte boundary) to the front, refill */
            uint32_t keep_from = pos & ~15u, keep = have - keep_from, n;
            memmove(rlbuf, rlbuf + keep_from, keep);
            pos -= keep_from; have = keep;
            n = RLBUF - have;
            n &= ~15u;
            if (n > left) n = left;
            fetch(rlbuf + have, ea, n);
            ea += n; left -= n; have += n;
        }
        if (nb == 0 && (pending & (1u << cur))) {   /* reuse: its earlier put must be done */
            DMA_WAIT(1 << (TAG_OUT0 + cur));
            pending &= ~(1u << cur);
        }
        p = (const unsigned short *)(rlbuf + pos);
        e = rl2blk(blk, p, job.iq_y, job.iq_uv);
        pos = (uint32_t)((const unsigned char *)e - rlbuf);
        if (bsize == 768) yuv2rgb24(blk, outbuf[cur] + nb * 768, bw);
        else yuv2rgb15(blk, (unsigned short *)(outbuf[cur] + nb * 512), stp, bw);
        nb++;
        if (nb == OUTMB || i + 1 == job.count) {
            DMA_PUT(outbuf[cur], job.out_ea + out_off, nb * bsize, TAG_OUT0 + cur);
            pending |= 1u << cur;
            out_off += nb * bsize;
            nb = 0; cur ^= 1;
        }
    }
    DMA_WAIT((1 << TAG_OUT0) | (1 << (TAG_OUT0 + 1)));
    /* the output is in memory: tell the PPU */
    done_word[0] = job.seq;
    DMA_PUT(done_word, job.done_ea, 16, TAG_DONE);
    DMA_WAIT(1 << TAG_DONE);
}

/* ---- anti-aliasing ---- */

#ifdef HOST_TEST
static uint32_t dec_now(void) { static uint32_t t = 0xffffffffu; return t -= 1000; }
#else
static uint32_t dec_now(void) { return spu_readch(SPU_RdDec); }   /* counts down */
#endif

static void put_done(uint64_t ea, uint32_t v)
{
    done_word[0] = v;
    DMA_PUT(done_word, ea, 16, TAG_DONE);
    DMA_WAIT(1 << TAG_DONE);
}

/* rows y-1 .. y+n (clamped) into work.a.in[0 .. n+1], padded */
static void aa_fetch(uint32_t y, uint32_t n)
{
    uint32_t k, nq = ajob.width / 4;
    for (k = 0; k < n + 2; k++) {
        int yy = (int)y + (int)k - 1;
        if (yy < 0) yy = 0;
        if (yy >= (int)ajob.height) yy = (int)ajob.height - 1;
        DMA_GET(&work.a.in[k][1], ajob.in_ea + (uint64_t)yy * ajob.pitch, ajob.width * 4, TAG_IN);
    }
    DMA_WAIT(1 << TAG_IN);
    for (k = 0; k < n + 2; k++) aa_pad((uint32_t *)work.a.in[k], (int)nq);
}

static void process_aa(void)
{
    uint32_t y, cur = 0, pending = 0, nq = ajob.width / 4, t0 = dec_now();
    static uint32_t lab[4] __attribute__((aligned(16)));
    if (nq > AA_Q || !nq || ajob.y1 > ajob.height) { put_done(ajob.done_ea, ajob.seq | 0x80000000u); return; }
    for (;;) {                                  /* the RSX has written the picture? */
        DMA_GET(lab, ajob.label_ea, 16, TAG_IN);
        DMA_WAIT(1 << TAG_IN);
        if (lab[0] == ajob.label_value) break;
        if (t0 - dec_now() > ajob.timeout) { put_done(ajob.done_ea, ajob.seq | 0x80000000u); return; }
    }
    for (y = ajob.y0; y < ajob.y1; y += AA_CH) {
        uint32_t n = ajob.y1 - y < AA_CH ? ajob.y1 - y : AA_CH, r;
        aa_fetch(y, n);
        if (pending & (1u << cur)) { DMA_WAIT(1 << (TAG_OUT0 + cur)); pending &= ~(1u << cur); }
        for (r = 0; r < n; r++) {
            aa_row(work.a.in[r], work.a.in[r + 1], work.a.in[r + 2], work.a.out[cur][r], (int)nq);
            DMA_PUT(work.a.out[cur][r], ajob.out_ea + (uint64_t)(y + r) * ajob.pitch, ajob.width * 4, TAG_OUT0 + cur);
        }
        pending |= 1u << cur;
        cur ^= 1;
    }
    DMA_WAIT((1 << TAG_OUT0) | (1 << (TAG_OUT0 + 1)));
    put_done(ajob.done_ea, ajob.seq);
}

/* mailbox word: MDEC job index + 1 (array at mdec_ea) or 0x100 + AA job index (at aa_ea) */
static void run_cmd(uint64_t mdec_ea, uint64_t aa_ea, uint32_t cmd)
{
    if (cmd >= 0x100) {
        DMA_GET(&ajob, aa_ea + (uint64_t)(cmd - 0x100) * sizeof(aa_job), sizeof ajob, TAG_IN);
        DMA_WAIT(1 << TAG_IN);
        process_aa();
        return;
    }
    DMA_GET(&job, mdec_ea + (uint64_t)(cmd - 1) * sizeof(mdec_job), sizeof job, TAG_IN);
    DMA_WAIT(1 << TAG_IN);
    process_job();
}

#ifdef HOST_TEST
void host_spu_mailbox(uint64_t mdec_ea, uint64_t aa_ea, uint32_t cmd) { run_cmd(mdec_ea, aa_ea, cmd); }
#else
int main(uint64_t mdec_ea, uint64_t aa_ea, uint64_t arg2, uint64_t arg3)
{
    (void)arg2; (void)arg3;
    for (;;) {
        uint32_t cmd = spu_read_in_mbox();
        if (!cmd) break;
        run_cmd(mdec_ea, aa_ea, cmd);
    }
    spu_thread_exit(0);
    return 0;
}
#endif
