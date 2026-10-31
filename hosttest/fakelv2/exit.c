#include <pthread.h>
#include <stdint.h>
void sysThreadExit(uint64_t r) { (void)r; pthread_exit(0); }
