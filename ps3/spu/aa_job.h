/* One anti-aliasing job for an SPU (ps3/src/spu_mdec.c side and the SPU program,
 * ps3/spu/mdec_spu.c): a band of rows of a picture in main memory that the RSX
 * writes; the SPU first waits for the RSX label saying the picture is complete. */
#ifndef AA_JOB_H
#define AA_JOB_H
#include <stdint.h>

#define AA_MAX_W 1920            /* widest picture (pixels, multiple of 4) */

typedef struct {
    uint64_t in_ea, out_ea;      /* pictures, rows `pitch` bytes apart (128-byte aligned) */
    uint64_t label_ea;           /* RSX label word (16-byte aligned) */
    uint64_t done_ea;            /* 16 bytes: the first word receives seq (| 0x80000000: gave up) */
    uint32_t width, height, pitch;
    uint32_t y0, y1;             /* this job's rows */
    uint32_t label_value;        /* wait until the label word equals this */
    uint32_t seq;
    uint32_t timeout;            /* decrementer ticks to wait for the label */
} __attribute__((aligned(128))) aa_job;

#endif
