/* One MDEC decoding job for an SPU (shared by the PPU side, ps3/src/spu_mdec.c,
 * and the SPU program, ps3/spu/mdec_spu.c). Lives in main memory, 128-byte
 * aligned; the SPU fetches it whole when its mailbox gets a non-zero word. */
#ifndef MDEC_JOB_H
#define MDEC_JOB_H
#include <stdint.h>

#define MDEC_JOB_RGB24  1     /* 24-bit output (768 bytes per macroblock), else 15-bit (512) */
#define MDEC_JOB_STP    2     /* 15-bit output: set bit 15 */
#define MDEC_JOB_BW     4     /* monochrome output */

/* largest run-length data of one macroblock: 6 blocks of a DC word and at most
   64 more words (the 64th always ends the block) */
#define MDEC_MAX_MB_BYTES (6 * 65 * 2)

typedef struct {
    uint64_t rl_ea;           /* run-length data, 16-byte aligned start */
    uint64_t out_ea;          /* output, 128-byte aligned */
    uint64_t done_ea;         /* 16 bytes: the first word receives seq when the output is in memory */
    uint32_t rl_skip;         /* bytes from rl_ea to the first macroblock */
    uint32_t rl_size;         /* bytes to fetch from rl_ea, multiple of 16 */
    uint32_t count;           /* macroblocks */
    uint32_t flags;           /* MDEC_JOB_* */
    uint32_t seq;
    uint32_t pad[3];
    int32_t iq_y[64], iq_uv[64];
} __attribute__((aligned(128))) mdec_job;

#endif
