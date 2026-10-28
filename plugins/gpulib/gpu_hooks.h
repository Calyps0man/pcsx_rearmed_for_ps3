/* Optional front-end hooks into the GPU plugin, used by the PS3 front-end's
 * hardware renderer to replay drawing on the RSX. All NULL by default, in
 * which case nothing changes. */
#ifndef __GPULIB_GPU_HOOKS_H__
#define __GPULIB_GPU_HOOKS_H__

#include <stdint.h>

struct gpu_frontend_hooks {
	/* a GP0 drawing/fill/state command, before the software renderer runs it;
	 * words are little-endian as in PS1 memory */
	void (*cmd)(const uint32_t *list, const uint32_t *list_end);
	/* VRAM area written outside drawing commands (uploads, copies, state load) */
	void (*vram_written)(int x, int y, int w, int h);
	/* drawing state registers E1..E6 reloaded (host-endian) */
	void (*sync_ecmds)(const uint32_t *ex_regs);
	/* show the display area; return nonzero if the front-end presented it,
	 * in which case the software frame is not sent */
	int (*present)(int x, int y, int w, int h, int bgr24);
};

extern struct gpu_frontend_hooks gpu_frontend_hooks;
/* native VRAM, 1024x512 little-endian halfwords */
const uint16_t *gpulib_get_vram(void);
/* drawing state registers, ex_regs[1..6] = E1..E6 (host-endian) */
const uint32_t *gpulib_get_ex_regs(void);
/* run the software renderer on its own thread (1) or inline (0); no effect
 * without USE_ASYNC_GPU. Off while front-end hooks are installed: they are
 * called from wherever the renderer runs. */
void gpulib_set_thread_rendering(int on);

/* time spent on the emulation thread, in timebase ticks (PowerPC only):
 * [0] GPU command processing and frame output (incl. waits for the GPU thread),
 * [1] SPU sample generation */
extern uint64_t gpulib_prof_ticks[2];
static inline uint64_t gpulib_prof_now(void)
{
#if defined(__powerpc__) || defined(__ppc__) || defined(__PPC__)
	return __builtin_ppc_get_timebase();
#else
	return 0;
#endif
}

#endif
