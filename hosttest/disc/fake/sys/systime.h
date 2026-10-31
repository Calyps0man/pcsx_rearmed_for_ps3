static void sysUsleep(unsigned long t) { (void)t; }
static unsigned long long fake_time;
static unsigned long long sysGetSystemTime(void) { return fake_time += 100; }
