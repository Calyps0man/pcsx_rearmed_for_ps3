/* Audio: the core produces 16-bit stereo at ~44.1 kHz; the PS3 audio port plays
 * 48 kHz float stereo in 256-sample blocks. A feeder thread wakes on each block
 * the port consumes and refills the block 4 ahead from a ring buffer, resampling
 * linearly. The resampling ratio is nudged by up to +-0.5% to keep the ring about
 * half full, so the emulator (paced by video) and the audio clock never drift
 * apart into crackles or growing latency.
 * "Slow down" mode (setting): when the emulator cannot keep full speed, the
 * sound is played slower (and lower) at the rate it is actually produced,
 * measured over half a second, instead of running dry and crackling. Within 5%
 * of full speed it plays exactly as in "Keep pitch" mode: the core delivers sound
 * once per frame (735 frames at a time), and measuring that every 5 ms swung by
 * +-15% at full speed, which played the sound slower and wobbling (GT2). */
#include <string.h>
#include <unistd.h>
#include <audio/audio.h>
#include <sys/thread.h>
#include <sys/event_queue.h>
#include <lv2/thread.h>
#include "common.h"

#define RING     8192                  /* stereo frames, power of two */
#define TARGET   (RING / 4)            /* aim for ~46 ms buffered */
#define AHEAD    4                     /* port blocks between play and write position */

static int16_t ring[RING * 2];
static volatile unsigned rd, wr;       /* free-running frame counters */
static volatile int running, paused;
static u32 port;
static audioPortConfig pcfg;
static sys_event_queue_t queue;
static sys_ipc_key_t qkey;
static sys_ppu_thread_t thread;
static double in_rate = 44100.0, pos;
static volatile unsigned underruns;  /* pos: fractional read position within the next frame */
static volatile int stretch;           /* slow-down mode */
static unsigned last_w, win_frames, win_blocks;   /* production measured over a window of blocks */
static double speed_target = 1.0, speed_now = 1.0; /* slow-down mode: playback speed (1 = full) */

void audio_set_stretch(int on) { stretch = on; }

unsigned audio_fill(void) { return wr - rd; }
static volatile unsigned underruns_total;
unsigned audio_underruns(void) { unsigned u = underruns; underruns = 0; return u; }
unsigned audio_underruns_total(void) { return underruns_total; }
void audio_set_input_rate(double hz) { if (hz > 8000.0) in_rate = hz; }
void audio_pause(int p) { paused = p; }

void audio_push(const int16_t *s, size_t n)
{
    unsigned w = wr, space = RING - (w - rd), i;
    if (n > space) n = space;              /* overrun: drop the newest samples */
    for (i = 0; i < n; i++) {
        unsigned k = (w + i) & (RING - 1);
        ring[k * 2] = s[i * 2]; ring[k * 2 + 1] = s[i * 2 + 1];
    }
    __asm__ volatile("lwsync" : : : "memory");
    wr = w + n;
}

static void fill_block(float *out)
{
    /* snapshot the producer position once; the barrier orders the ring reads after it */
    unsigned w = wr, r = rd, avail, i;
    double ratio, step;
    __asm__ volatile("lwsync" : : : "memory");
    avail = w - r;
    {
        /* production rate over about half a second (96 blocks); a window with the
           menu open is not counted */
        win_frames += w - last_w; last_w = w;
        if (paused) win_frames = win_blocks = 0;
        else if (++win_blocks >= 96) {
            double sp = (double)win_frames * 48000.0 / (win_blocks * (double)AUDIO_BLOCK_SAMPLES) / in_rate;
            /* near full speed: full speed (the measurement is +-3% coarse); far below
               half: a stall, a load or muted fast-forwarding, not a slow game */
            speed_target = sp >= 0.95 || sp < 0.45 ? 1.0 : sp < 0.5 ? 0.5 : sp;
            win_frames = win_blocks = 0;
        }
        speed_now += (speed_target - speed_now) * 0.03;    /* glides over ~0.2 s */
    }
    if (stretch && speed_now < 0.999) {
        /* play at the rate the game produces (50..95%), and correct the buffer level
           by up to +-2% around that */
        ratio = 1.0 + 0.02 * ((double)avail - TARGET) / TARGET;
        if (ratio < 0.98) ratio = 0.98;
        if (ratio > 1.02) ratio = 1.02;
        ratio *= speed_now;
    } else {
        ratio = 1.0 + 0.005 * ((double)avail - TARGET) / TARGET;
        if (ratio < 0.995) ratio = 0.995;
        if (ratio > 1.005) ratio = 1.005;
    }
    step = in_rate / 48000.0 * ratio;
    for (i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
        if (paused || w - r < 2) {
            if (!paused && i == 0) { underruns++; underruns_total++; }
            out[i * 2] = out[i * 2 + 1] = 0.f; continue;
        }
        {
            unsigned a = r & (RING - 1), b = (r + 1) & (RING - 1);
            float f = (float)pos;
            out[i * 2]     = (ring[a * 2]     + (ring[b * 2]     - ring[a * 2])     * f) * (1.f / 32768.f);
            out[i * 2 + 1] = (ring[a * 2 + 1] + (ring[b * 2 + 1] - ring[a * 2 + 1]) * f) * (1.f / 32768.f);
        }
        pos += step;
        while (pos >= 1.0) { pos -= 1.0; r++; }
    }
    /* ring reads complete before the producer may reuse the slots */
    __asm__ volatile("lwsync" : : : "memory");
    rd = r;
}

static void feeder(void *arg)
{
    float *base = (float *)(uintptr_t)pcfg.audioDataStart;
    u64 *read_index = (u64 *)(uintptr_t)pcfg.readIndex;
    u64 nb = pcfg.numBlocks, filled = 0, prev_ri;
    (void)arg;
    /* blocks are counted monotonically; the port's index wraps every nb blocks */
    prev_ri = *read_index;
    filled = prev_ri + AHEAD;
    while (running) {
        sys_event_t ev;
        u64 ri = *read_index, played, target;
        if (sysEventQueueReceive(queue, &ev, 100 * 1000) != 0 && !running) break;
        ri = *read_index;
        played = (ri + nb - prev_ri) % nb;        /* blocks consumed since the last wake-up */
        prev_ri = ri;
        target = filled + played;
        if (target - filled > nb - 1) target = filled + nb - 1;
        /* refill every block between our last write and AHEAD past the play position,
           so a late wake-up never leaves an old block to be replayed */
        while (filled < target) {
            filled++;
            fill_block(base + (filled % nb) * 2 * AUDIO_BLOCK_SAMPLES);
        }
    }
    sysThreadExit(0);
}

int audio_init(void)
{
    audioPortParam pp;
    if (audioInit() != 0) return 0;
    memset(&pp, 0, sizeof pp);
    pp.numChannels = AUDIO_PORT_2CH;
    pp.numBlocks = AUDIO_BLOCK_8;
    pp.attrib = 0;
    pp.level = 1.0f;
    if (audioPortOpen(&pp, &port) != 0) return 0;
    if (audioGetPortConfig(port, &pcfg) != 0) return 0;
    if (audioCreateNotifyEventQueue(&queue, &qkey) != 0) return 0;
    if (audioSetNotifyEventQueue(qkey) != 0) return 0;
    sysEventQueueDrain(queue);
    memset((void *)(uintptr_t)pcfg.audioDataStart, 0, pcfg.numBlocks * 2 * AUDIO_BLOCK_SAMPLES * sizeof(float));
    running = 1;
    if (sysThreadCreate(&thread, feeder, NULL, 100, 0x4000, THREAD_JOINABLE, (char *)"audio") != 0) return 0;
    audioPortStart(port);
    logf_("audio: port %u, %u blocks\n", (unsigned)port, (unsigned)pcfg.numBlocks);
    return 1;
}

void audio_shutdown(void)
{
    u64 rv;
    if (!running) return;
    running = 0;
    sysThreadJoin(thread, &rv);
    audioPortStop(port);
    audioRemoveNotifyEventQueue(qkey);
    audioPortClose(port);
    sysEventQueueDestroy(queue, 0);
    audioQuit();
}
