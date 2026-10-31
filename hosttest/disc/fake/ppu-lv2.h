#include <stdint.h>
typedef int32_t s32;
typedef uint64_t u64;
static uint64_t fake_syscall(unsigned, uint64_t *);
#define U(x) ((uint64_t)(uintptr_t)(x))
#define SC(n,...) uint64_t fake_args[7] = {__VA_ARGS__}; uint64_t fake_ret = fake_syscall(n, fake_args); (void)fake_ret
#define lv2syscall1(n,a) SC(n,U(a))
#define lv2syscall2(n,a,b) SC(n,U(a),U(b))
#define lv2syscall3(n,a,b,c) SC(n,U(a),U(b),U(c))
#define lv2syscall4(n,a,b,c,d) SC(n,U(a),U(b),U(c),U(d))
#define lv2syscall6(n,a,b,c,d,e,f) SC(n,U(a),U(b),U(c),U(d),U(e),U(f))
#define lv2syscall7(n,a,b,c,d,e,f,g) SC(n,U(a),U(b),U(c),U(d),U(e),U(f),U(g))
#define return_to_user_prog(t) return (t)fake_ret
