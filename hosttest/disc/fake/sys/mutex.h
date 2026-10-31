typedef unsigned sys_mutex_t;
typedef struct { int attr_protocol, attr_recursive; } sys_mutex_attr_t;
#define SYS_MUTEX_PROTOCOL_FIFO 1
#define SYS_MUTEX_ATTR_NOT_RECURSIVE 0
static int sysMutexLock(sys_mutex_t l, unsigned long t) { (void)l; (void)t; return 0; }
static int sysMutexUnlock(sys_mutex_t l) { (void)l; return 0; }
static int sysMutexDestroy(sys_mutex_t l) { (void)l; return 0; }
static int sysMutexCreate(sys_mutex_t *l, sys_mutex_attr_t *a) { (void)a; *l=1; return 0; }
