/* host UI tests: a clock that advances 20 ms per call */
static unsigned long long ui_fake_time;
static unsigned long long sysGetSystemTime(void) { return ui_fake_time += 20000; }
