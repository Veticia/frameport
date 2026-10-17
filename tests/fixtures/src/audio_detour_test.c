// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
static _Alignas(4096) unsigned char entry[4096],arena[4096];
static unsigned maps,unmaps,protects,logs,fail_at;
static int outside,missing,restore_fails;
static void *slot;
static const uint32_t expected[4]={0xd10243ff,0xa9065ffe,0xa90757f6,0xa9084ff4};
static void *fake_mmap(void *hint,size_t size,int prot,int flags,int fd,off_t offset) {
    (void)hint;(void)size;(void)prot;(void)flags;(void)fd;(void)offset;
    maps++;
    if(missing)return MAP_FAILED;
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
    return protects==fail_at || (restore_fails && protects>=3)?-1:0;
}
#define mmap fake_mmap
#define munmap fake_munmap
#define mprotect fake_mprotect
#define LOG(...) ((void)logs++)
#include "audio_detour.h"
static void reset(void) {
    maps=unmaps=protects=0;fail_at=0;outside=missing=restore_fails=0;slot=NULL;
    memcpy(entry,expected,16);memset(arena,0,sizeof(arena));mx_audio_page=4096;
}
int main(void) {
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
    assert(maps>1 && unmaps==maps && !protects && !memcmp(entry,expected,16));
    reset();missing=1;
    assert(!mx_audio_detour(entry,(void*)16,expected,&slot));
    assert(maps>1 && !unmaps && !protects && !memcmp(entry,expected,16));
    reset();entry[4]^=1;
    assert(!mx_audio_detour(entry,(void*)16,expected,&slot) && !maps);
    reset();uint32_t bad[4]={0x10000000,0,0,0};memcpy(entry,bad,16);
    assert(!mx_audio_detour(entry,(void*)16,bad,&slot) && !maps);
    assert(logs==1);
    puts("PASS: branch limits, stub/trampoline, publication order, failure cleanup/rollback, guards");
}
