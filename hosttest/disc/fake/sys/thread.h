static void sysThreadYield(void) {}
/* the game list's identification thread: run inline */
typedef unsigned long long sys_ppu_thread_t;
#define THREAD_JOINABLE 1
static int sysThreadCreate(sys_ppu_thread_t *t, void (*f)(void *), void *a, int pr, unsigned long long st, int fl, char *nm)
{ (void)pr; (void)st; (void)fl; (void)nm; *t = 1; f(a); return 0; }
static int sysThreadJoin(sys_ppu_thread_t t, u64 *r) { (void)t; *r = 0; return 0; }
static void sysThreadExit(unsigned long long r) { (void)r; }
