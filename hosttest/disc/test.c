/* Exercise the real reader with a simulated LV1/LV2/ATAPI boundary. */
#include <assert.h>
#include <stdarg.h>
#include "../../ps3/src/disc.c"

static uint64_t hv[4], flag, kernel;
static const uint64_t originals[4] = {
    UINT64_C(0x7f83e378f8010098), UINT64_C(0x7f85e3784bfff0e5),
    UINT64_C(0x7f84e37838a10070), UINT64_C(0x9be1007048005fa5)
};
static int writes, closes, require_patch, require_mode, mode, q_only, bad_toc,
    fail_write, auth_fail, always_fail, busy, audio2, fail_lba = -1, multi_fail, multi_bad, speed_sets;
static uint64_t free_space = UINT64_C(1) << 40;
static uint64_t kernel_addr = UINT64_C(0x80000000002ed818);
static uint64_t flag_addr = UINT64_C(0x80000000002ecab0);
void logf_(const char *fmt, ...) { (void)fmt; }
void log_sync(void) {}

static uint64_t mock_peek(int level, uint64_t addr)
{
    int i;
    if (level == 2) {
        if (addr == UINT64_C(0x8000000000003000)) {
            if (kernel_addr == UINT64_C(0x80000000002ed818)) return UINT64_C(0x800000000034fbb0);
            if (kernel_addr == UINT64_C(0x80000000002ed808)) return UINT64_C(0x800000000034fba0);
            if (kernel_addr == UINT64_C(0x800000000030f3b0)) return UINT64_C(0x80000000003759c0);
        }
        if (addr == kernel_addr) return kernel;
        if (addr == flag_addr) return flag;
        return 0;
    }
    for (i=0;i<4;i++) if (addr == (uint64_t[]){0x16fa60,0x16fa84,0x16fafc,0x16fb04}[i]) return hv[i];
    return 0;
}
static void mock_poke(int level, uint64_t addr, uint64_t val)
{
    int i;
    ++writes;
    if (writes == fail_write) return;
    if (level == 2 && addr == flag_addr) { flag=val; return; }
    for (i=0;i<4;i++) if (addr == (uint64_t[]){0x16fa60,0x16fa84,0x16fafc,0x16fb04}[i]) { hv[i]=val; return; }
    assert(!"unexpected memory write");
}
static int unlocked(void) { return (uint32_t)hv[0] == 0x60000000 && (uint32_t)(flag >> 32) == 0x40000000; }
static void reset(void)
{
    memcpy(hv, originals, sizeof hv);
    flag=UINT64_C(0x0100000012345678); kernel=UINT64_C(0x4345580000000000);
    kernel_addr=UINT64_C(0x80000000002ed818); flag_addr=UINT64_C(0x80000000002ecab0);
    writes=closes=require_patch=require_mode=mode=q_only=bad_toc=fail_write=auth_fail=always_fail=busy=audio2=multi_fail=multi_bad=speed_sets=0; fail_lba=-1; free_space=UINT64_C(1)<<40;
}
static void restored(void)
{
    assert(!memcmp(hv, originals, sizeof hv));
    assert(flag == UINT64_C(0x0100000012345678));
}
/* one raw sector: a small ISO 9660 tree for the serial (PVD 16 -> root 22 -> SYSTEM.CNF 23),
   every sector carrying its LBA at bytes 2000..2003, audio sectors a pattern */
static void fake_sector(uint32_t lba, uint8_t *b, int with_q)
{
    uint8_t *dd = b + 24;
    memset(b, 0, with_q ? 2368 : 2352); memset(b+1,0xff,10); b[15]=2;
    if (audio2 && lba >= 3000) { memset(b, 0x5a, 2352); memcpy(b + 2000, &lba, 4); goto done; }
    memcpy(b + 2000, &lba, 4);
    if (lba == 22) {
        uint8_t *r = dd; r[0] = 34; r[32] = 1;                  /* "." */
        r = dd + 34; r[0] = 33 + 12 + 1; r[2] = 23; r[10] = 0x80; r[32] = 12; memcpy(r + 33, "SYSTEM.CNF;1", 12);
        goto done;
    }
    if (lba == 23) { strcpy((char *)dd, "BOOT = cdrom:\\SLUS_007.07;1\r\nTCB = 4\r\n"); goto done; }
    dd[156 + 2] = 22; dd[156 + 10 + 1] = 8;                    /* root: LBA 22, 2048 bytes */
    memcpy(b+25,"CD001",5); memset(b+64,' ',32); memcpy(b+64,"PHYSICAL CD TEST",16);
done:
    if (with_q) memset(b+2352, 0xab, 16);
}
static uint64_t fake_syscall(unsigned n, uint64_t *a)
{
    struct atapi_cmd *c;
    uint8_t *b;
    if (n==6 || n==8) return mock_peek(n==8 ? 1:2, a[0]);
    if (n==7 || n==9) { mock_poke(n==9 ? 1:2, a[0], a[1]); return 0; }
    if (n==600) { *(uint32_t *)(uintptr_t)a[2]=0; return 0; } /* fd=0 is valid */
    if (n==601) { ++closes; return 0; }
    if (n==840) { *(uint64_t *)(uintptr_t)a[1]=free_space; *(uint64_t *)(uintptr_t)a[2]=free_space; return 0; }
    if (n==864) {
        assert(a[0]==0x5004);
        if (auth_fail) return 0x80010505;
        mode=(int)a[1]; return 0;
    }
    assert(n==604 || n==616);
    assert(a[1]==1 && a[3]==56);
    c=(struct atapi_cmd *)(uintptr_t)a[2]; b=(uint8_t *)(uintptr_t)a[4];
    assert(c->pktlen==12 && c->blocks==1 && c->block_size==a[5] && c->in_out==1);
    if (n==616) *(uint64_t *)(uintptr_t)a[6]=0;
    if (c->pkt[0]==0x43) {
        assert(a[5]==804);
        if (audio2) {   /* data track 1, audio track 2 at 3000 (0x0bb8), leadout 4096 */
            b[1]=26; b[2]=1; b[3]=2;
            b[5]=0x14; b[6]=1;
            b[13]=0x10; b[14]=2; b[18]=0x0b; b[19]=0xb8;
            b[21]=0x10; b[22]=0xaa; b[26]=0x10;
            return 0;
        }
        b[1]=bad_toc ? 255 : 18; b[2]=b[3]=1;
        b[5]=0x14; b[6]=1;
        b[13]=0x10; b[14]=0xaa; b[18]=0x10; /* leadout 4096 */
        return 0;
    }
    if (c->pkt[0]==3) return 0;
    if (c->pkt[0]==0xbb) { assert(a[5]==0); ++speed_sets; return 0; }   /* SET CD SPEED */
    assert(c->pkt[0]==0xbe && c->pkt[6]==0 && c->pkt[7]==0 && c->pkt[8]>=1 && c->pkt[9]==0xf8);
    if (busy) { --busy; return 0x8001000a; }
    if (always_fail || (require_patch && !unlocked()) || (require_mode && mode!=require_mode)) return (uint64_t)-1;
    if (q_only && c->pkt[10]!=2) return (uint64_t)-1;
    {
        unsigned nsec = c->pkt[8], ss = c->pkt[10]==2 ? 2368 : 2352, k;
        uint32_t lba0 = (uint32_t)c->pkt[2] << 24 | c->pkt[3] << 16 | c->pkt[4] << 8 | c->pkt[5];
        assert(a[5]==nsec*ss);
        if (nsec > 1 && multi_fail) return (uint64_t)-1;
        for (k = 0; k < nsec; k++) {
            if ((int)(lba0 + k) == fail_lba) return (uint64_t)-1;
            fake_sector(lba0 + k, b + k * ss, c->pkt[10]==2);
            if (nsec > 1 && multi_bad) b[k * ss + 100] ^= 1;
        }
    }
    return 0;
}
int main(void)
{
    disc_t *d; disc_access a; char label[64]; unsigned char raw[2353]; int i;
    char serial[16];
    /* the game list's identification: serial from SYSTEM.CNF; never patches */
    reset(); d=disc_open(label,sizeof label,0); assert(d && !writes);
    assert(!read_serial(d,serial,sizeof serial) && !strcmp(serial,"SLUS00707")); disc_free(d);
    reset(); require_patch=1; d=disc_open(label,sizeof label,0); assert(!d && !writes); restored();
    {   /* the background check (run inline here): no title list on the host -> the label */
        char t[128];
        reset(); disc_identify_start(); assert(disc_identify_result(t,sizeof t)==1 && !strcmp(t,"PHYSICAL CD TEST"));
        /* plain reads blocked (this console): the check unlocks like starting it does, and restores */
        reset(); require_patch=1; require_mode=0x29; disc_identify_start();
        assert(disc_identify_result(t,sizeof t)==1 && !strcmp(t,"PHYSICAL CD TEST")); restored();
        /* nothing readable even then: a CD it could not read */
        reset(); always_fail=1; disc_identify_start(); assert(disc_identify_result(t,sizeof t)==2); restored();
    }
    reset(); d=disc_open(label,sizeof label,1); assert(d && !strcmp(label,"PHYSICAL CD TEST"));
    assert(!writes); disc_free(d); assert(closes==1); restored();
    reset(); q_only=1; d=disc_open(NULL,0,1); assert(d && d->big==READ_604_Q);
    memset(raw,0xcc,sizeof raw); assert(!read_raw(d,17,raw)); assert(raw[2352]==0xcc); disc_free(d); assert(!writes);
    reset(); require_patch=1; require_mode=0x29; d=disc_open(NULL,0,1);
    assert(d && unlocked() && mode==0x29); disc_free(d); restored(); assert(writes==10);
    reset(); require_patch=1; require_mode=1; d=disc_open(NULL,0,1);
    assert(d && mode==1); disc_free(d); assert(mode==0x29); restored();
    reset(); require_patch=1; always_fail=1; auth_fail=1; assert(!disc_open(NULL,0,1)); restored();
    reset(); require_patch=1; kernel=0; assert(!disc_open(NULL,0,1)); assert(!writes); restored();
    reset(); require_patch=1; hv[2]=0; assert(!disc_open(NULL,0,1)); assert(!writes);
    reset(); bad_toc=1; assert(!disc_open(NULL,0,1)); assert(!writes && closes==1);
    reset(); busy=3; d=disc_open(NULL,0,1); assert(d && !busy); disc_free(d);
    reset(); kernel_addr=UINT64_C(0x80000000002ed808); flag_addr=UINT64_C(0x80000000002ecaa0);
    assert(!disc_access_enable(&a,mock_peek,mock_poke)); assert(!disc_access_restore(&a,mock_peek,mock_poke)); restored();
    reset(); kernel_addr=UINT64_C(0x800000000030f3b0); flag_addr=UINT64_C(0x800000000030e648); kernel=UINT64_C(0x4445580000000000);
    assert(!disc_access_enable(&a,mock_peek,mock_poke)); assert(!disc_access_restore(&a,mock_peek,mock_poke)); restored();
    for(i=1;i<=5;i++) {
        reset(); fail_write=i; assert(disc_access_enable(&a,mock_peek,mock_poke)==-4); restored();
    }
    reset(); assert(!disc_access_enable(&a,mock_peek,mock_poke));
    { disc_access b; int count=writes; assert(!disc_access_enable(&b,mock_peek,mock_poke)); assert(writes==count);
      assert(!disc_access_restore(&b,mock_peek,mock_poke)); assert(writes==count); }
    assert(!disc_access_restore(&a,mock_peek,mock_poke)); restored();
    reset(); assert(!disc_access_enable(&a,mock_peek,mock_poke)); hv[2]=123;
    assert(disc_access_restore(&a,mock_peek,mock_poke)==-1); assert(hv[2]==123);
    /* Regression: an unanticipated permission word must not reject the known
     * layout, and the adjacent lower 32 bits must survive enable/restore. */
    reset(); flag=UINT64_C(0xdeadbeef12345678);
    assert(!disc_access_enable(&a,mock_peek,mock_poke));
    assert(flag==UINT64_C(0x4000000012345678));
    assert(!disc_access_restore(&a,mock_peek,mock_poke));
    assert(flag==UINT64_C(0xdeadbeef12345678));
    /* Real log's LV1 fourth-site branch differs from the older source. */
    reset(); hv[3]=UINT64_C(0x9be1007048006065);
    assert(!disc_access_enable(&a,mock_peek,mock_poke));
    assert(!disc_access_restore(&a,mock_peek,mock_poke));
    assert(hv[3]==UINT64_C(0x9be1007048006065));
    {   /* copying the disc to the HDD */
        struct disc_copy *cp; char err[160]; uint32_t dn, tt; int r, k; FILE *f; long sz;
        unsigned char sec[2352]; char cue[512];
        const char *dir = "/tmp/disc_copy_test";
        system("rm -rf /tmp/disc_copy_test");
        int variant;
        for (variant = 0; variant < 4; variant++) {   /* batches; refused; wrong data; with Q */
        system("rm -rf /tmp/disc_copy_test");
        reset(); audio2=1; multi_fail = variant == 1; multi_bad = variant == 2; q_only = variant == 3;
        cp=disc_copy_begin(dir,"Test Game (USA)",err,sizeof err); assert(cp);
        while ((r = disc_copy_step(cp, 5, &dn, &tt)) == 1) assert(dn <= tt && tt == 4096);
        assert(r == 0); assert(cp->multi == (variant == 1 || variant == 2 ? -1 : 1)); assert(speed_sets >= 1); disc_copy_end(cp); restored();
        f=fopen("/tmp/disc_copy_test/Test Game (USA).bin","rb"); assert(f);
        fseek(f,0,SEEK_END); sz=ftell(f); assert(sz == 4096L*2352);
        for (k = 0; k < 4096; k += 7) {
            uint32_t l; fseek(f,(long)k*2352,SEEK_SET); assert(fread(sec,1,2352,f)==2352);
            memcpy(&l, sec+2000, 4); assert(l == (uint32_t)k);
            assert(k < 3000 ? sec[15]==2 && sec[1]==0xff : sec[0]==0x5a);
        }
        fclose(f);
        f=fopen("/tmp/disc_copy_test/Test Game (USA).cue","rb"); assert(f);
        k=(int)fread(cue,1,sizeof cue-1,f); cue[k]=0; fclose(f);
        assert(!strcmp(cue,"FILE \"Test Game (USA).bin\" BINARY\r\n  TRACK 01 MODE2/2352\r\n    INDEX 01 00:00:00\r\n"
                           "  TRACK 02 AUDIO\r\n    INDEX 01 00:40:00\r\n"));
        assert(!fopen("/tmp/disc_copy_test/Test Game (USA).bin.part","rb"));
        }
        /* already there: not copied again */
        reset(); assert(!disc_copy_begin(dir,"Test Game (USA)",err,sizeof err) && strstr(err,"Already"));
        /* not enough space */
        reset(); free_space=4096L*2352; assert(!disc_copy_begin(dir,"Other",err,sizeof err) && strstr(err,"space")); restored();
        /* an unreadable sector: fails, the partial file is removed, patches undone */
        reset(); require_patch=1; require_mode=0x29; fail_lba=2000; cp=disc_copy_begin(dir,"Bad",err,sizeof err); assert(cp);
        while ((r = disc_copy_step(cp, 5, &dn, &tt)) == 1) ;
        assert(r == -1 && strstr(disc_copy_error(cp),"2000")); disc_copy_end(cp); restored();
        assert(!fopen("/tmp/disc_copy_test/Bad.bin.part","rb") && !fopen("/tmp/disc_copy_test/Bad.bin","rb"));
        /* cancelled half way */
        reset(); cp=disc_copy_begin(dir,"Cancel",err,sizeof err); assert(cp);
        assert(disc_copy_step(cp, 1, &dn, &tt) == 1 && dn < tt); disc_copy_end(cp);
        assert(!fopen("/tmp/disc_copy_test/Cancel.bin.part","rb") && !fopen("/tmp/disc_copy_test/Cancel.cue","rb"));
        system("rm -rf /tmp/disc_copy_test");
    }
    puts("disc: 29 simulated read/access/cleanup/copy scenarios passed");
    return 0;
}
