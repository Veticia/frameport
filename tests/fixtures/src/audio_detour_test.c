// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
static _Alignas(4096) unsigned char entry[4096],arena[4096];
static unsigned maps,unmaps,protects,logs,fail_at;
static int outside,missing,restore_fails;
static uintptr_t hints[3];
static char last_log[1024];
static void *slot;
static const uint32_t expected[4]={0xd10243ff,0xa9065ffe,0xa90757f6,0xa9084ff4};
static void *fake_mmap(void *hint,size_t size,int prot,int flags,int fd,off_t offset) {
    (void)size;(void)prot;(void)fd;(void)offset;
    assert(flags & 0x100000);
    if(!missing)assert(hint==arena);
    assert(maps<3);hints[maps]=(uintptr_t)hint;
    maps++;
    if(missing){errno=maps==1?EEXIST:maps==2?EINVAL:ENOMEM;return MAP_FAILED;}
    return outside?(void*)((uintptr_t)entry+0x10000000):arena;
}
static int fake_munmap(void *p,size_t size) {
    (void)p;assert(size==4096);unmaps++;return 0;
}
static int fake_mprotect(void *p,size_t size,int prot) {
    assert(size==4096);protects++;
    if(protects==1) {
        assert(p==arena && prot==(PROT_READ|PROT_EXEC));
        assert(!memcmp(entry,expected,16));
    }
    if(protects==3) {
        assert(p==entry && prot==(PROT_READ|PROT_EXEC));
        assert(slot==arena+16); // original is available when entry goes live
        assert(*(uint32_t*)entry!=expected[0]);
        assert(!memcmp(entry+4,expected+1,12));
    }
    if(protects==4)assert(!memcmp(entry,expected,16));
    if(protects==fail_at || (restore_fails && protects>=3)){errno=EACCES;return -1;}
    return 0;
}
static FILE *fake_fopen(const char *path,const char *mode) {
    (void)path;(void)mode;
    FILE *f=tmpfile();assert(f);
    fprintf(f,"0-%llx r-xp\n%llx-ffffffffffffffff r-xp\n",
            (unsigned long long)(uintptr_t)arena,
            (unsigned long long)((uintptr_t)arena+(missing?12288:4096)));
    rewind(f);return f;
}
static void test_log(const char *format,...) {
    char message[1024];va_list ap;va_start(ap,format);
    vsnprintf(message,sizeof(message),format,ap);va_end(ap);
    assert(strstr(message,"target="));strcpy(last_log,message);logs++;
}
#define fopen fake_fopen
#define mmap fake_mmap
#define munmap fake_munmap
#define mprotect fake_mprotect
#define LOG(...) test_log(__VA_ARGS__)
#include "audio_detour.h"
static void reset(void) {
    maps=unmaps=protects=0;fail_at=0;outside=missing=restore_fails=0;slot=NULL;
    memcpy(entry,expected,16);memset(arena,0,sizeof(arena));mx_audio_page=4096;
}
static void gap_test(const char *text,uintptr_t target,size_t expected_count,uintptr_t first) {
    FILE *f=tmpfile();assert(f);fputs(text,f);rewind(f);
    struct mx_audio_candidate *c=NULL;size_t n=0;
    const char *failure=NULL;
    assert(mx_audio_candidates(f,target,&c,&n,&failure));fclose(f);
    assert(n==expected_count);if(n)assert(c[0].address==first);
    for(size_t i=0;i<n;i++){uint32_t w;assert(mx_audio_branch(target,c[i].address,&w));assert(mx_audio_branch(c[i].address+20,target+4,&w));}
    free(c);
}
int main(void) {
    mx_audio_page=4096;
    gap_test("0-10000000 r-xp\n10003000-ffffffffffffffff r-xp\n",0x10001000,3,0x10001000);
    gap_test("0-10000000 r-xp\n10001000-10002000 r-xp\n10002000-ffffffffffffffff r-xp\n",0x10002000,1,0x10000000);
    gap_test("0-20000000 r-xp\n20001000-ffffffffffffffff r-xp\n",0x10000000,0,0);
    gap_test("0-10000000 r-xp\n10000000-ffffffffffffffff r-xp\n",0x10000000,0,0);

    gap_test("0-18000000 r-xp\n18001000-ffffffffffffffff r-xp\n",0x10000004,0,0);
    uint32_t b;
    assert(mx_audio_branch(0x10000000,0x10000000,&b) && b==0x14000000);
    assert(mx_audio_branch(0x10000000,0x17fffffc,&b) && b==0x15ffffff);
    assert(mx_audio_branch(0x10000000,0x08000000,&b) && b==0x16000000);
    assert(mx_audio_branch(0x10000004,0x10000000,&b) && b==0x17ffffff);
    assert(!mx_audio_branch(0x10000000,0x18000000,&b));
    assert(!mx_audio_branch(0x10000004,0x08000000,&b));
    assert(!mx_audio_branch(1,4,&b) && !mx_audio_branch(4,1,&b));
    reset();
    assert(mx_audio_detour(entry,(void*)0x123456789abcdef0ULL,expected,&slot)==arena+16);
    uint32_t jump[2]={0x58000050,0xd61f0200};uintptr_t destination;
    assert(!memcmp(arena,jump,8));memcpy(&destination,arena+8,8);
    assert(destination==0x123456789abcdef0ULL);
    assert(*(uint32_t*)(arena+16)==expected[0]);
    assert(mx_audio_branch((uintptr_t)arena+20,(uintptr_t)entry+4,&b));
    assert(*(uint32_t*)(arena+20)==b && maps==1 && !unmaps && protects==3);
    for(unsigned failure=1;failure<=3;failure++) {
        reset();fail_at=failure;
        assert(!mx_audio_detour(entry,(void*)16,expected,&slot));
        assert(strstr(last_log,failure==1?"mprotect stub RX":failure==2?"mprotect entry RWX":"mprotect entry RX"));
        assert(strstr(last_log,"errno=13"));
        assert(!memcmp(entry,expected,16));
        assert(unmaps==(failure<3?1u:0u));
        assert(slot==(failure<3?NULL:arena+16));
        assert(protects==(failure==3?4:failure));
    }
    reset();restore_fails=1;
    assert(!mx_audio_detour(entry,(void*)16,expected,&slot));
    assert(!memcmp(entry,expected,16) && !unmaps && protects==4 && slot==arena+16);
    reset();outside=1;
    assert(!mx_audio_detour(entry,(void*)16,expected,&slot));
    assert(maps==1 && unmaps==maps && !protects && !memcmp(entry,expected,16));
    reset();missing=1;
    assert(!mx_audio_detour(entry,(void*)16,expected,&slot));
    assert(maps==3 && !unmaps && !protects && !memcmp(entry,expected,16));
    for(unsigned i=1;i<3;i++) {
        uintptr_t t=(uintptr_t)entry;
        uintptr_t a=hints[i-1]>t?hints[i-1]-t:t-hints[i-1];
        uintptr_t b=hints[i]>t?hints[i]-t:t-hints[i];
        assert(a<=b && hints[i]!=hints[i-1]);
    }
    assert(strstr(last_log,"errno=12 attempts=3"));
    reset();entry[4]^=1;
    assert(!mx_audio_detour(entry,(void*)16,expected,&slot) && !maps);
    assert(strstr(last_log,"16-byte guard mismatch"));
    reset();uint32_t bad[4]={0x10000000,0,0,0};memcpy(entry,bad,16);
    assert(!mx_audio_detour(entry,(void*)16,bad,&slot) && !maps);
    assert(strstr(last_log,"unsupported replay instruction"));
    assert(logs==8);
    puts("PASS: maps gaps, outward NOREPLACE retries, branch limits, stub/trampoline, publication order, failure diagnostics/cleanup/rollback, guards");
}
