// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#include <string.h>
#include <sys/mman.h>

static size_t mx_audio_page;
static int mx_audio_branch(uintptr_t from,uintptr_t to,uint32_t *word) {
    if((from|to)&3)return 0;
    if(to>=from) {if(to-from>=((uintptr_t)1<<27))return 0;}
    else if(from-to>((uintptr_t)1<<27))return 0;
    *word=0x14000000u|((uint32_t)((to-from)>>2)&0x03ffffffu);
    return 1;
}
static void mx_audio_jump(unsigned char *out,uintptr_t destination) {
    const uint32_t jump[2]={0x58000050u,0xd61f0200u};
    memcpy(out,jump,8);memcpy(out+8,&destination,8);
}
struct mx_audio_candidate { uintptr_t address,distance; };
static int mx_audio_candidate_order(const void *a,const void *b) {
    const struct mx_audio_candidate *x=a,*y=b;
    if(x->distance!=y->distance)return x->distance<y->distance?-1:1;
    return x->address<y->address?-1:x->address>y->address;
}
// Maps are sorted by address. Clip every free gap to the intersection of both
// branch ranges; enumerate pages, then sort outward (lower address breaks ties).
static int mx_audio_candidates(FILE *maps,uintptr_t target,
                               struct mx_audio_candidate **out,size_t *count,const char **failure) {
    uintptr_t radius=(uintptr_t)1<<27;
    uintptr_t low=target>radius?target-radius:0;
    uintptr_t high=target>UINTPTR_MAX-radius?UINTPTR_MAX:target+radius;
    uintptr_t cursor=low;
    size_t capacity=0,line_capacity=0;char *line=NULL;
    *out=NULL;*count=0;
    for(;;) {
        unsigned long long start,end;
        *failure="read /proc/self/maps";
        int eof=getline(&line,&line_capacity,maps)<0;
        if(eof && (ferror(maps) || !feof(maps)))goto fail;
        if(!eof && (sscanf(line,"%llx-%llx",&start,&end)!=2 || end<=start)) {*failure="parse /proc/self/maps";errno=EINVAL;goto fail;}
        uintptr_t stop=eof?high:(uintptr_t)start;
        if(stop>high)stop=high;
        uintptr_t page=cursor;
        if(page%mx_audio_page)page+=mx_audio_page-page%mx_audio_page;
        for(;page<stop && stop-page>=mx_audio_page;page+=mx_audio_page) {
            uint32_t word;
            if(!page || !mx_audio_branch(target,page,&word) ||
               !mx_audio_branch(page+20,target+4,&word))continue;
            if(*count==capacity) {
                size_t next=capacity?capacity*2:64;
                void *p=realloc(*out,next*sizeof(**out));
                if(!p){*failure="allocate stub candidates";goto fail;}
                *out=p;capacity=next;
            }
            (*out)[(*count)++]=(struct mx_audio_candidate){page,page>target?page-target:target-page};
        }
        if(eof || stop==high)break;
        if((uintptr_t)end>cursor)cursor=(uintptr_t)end;
        if(cursor>=high)break;
    }
    free(line);
    if(*count)qsort(*out,*count,sizeof(**out),mx_audio_candidate_order);
    return 1;
fail:
    {int saved=errno;free(line);free(*out);*out=NULL;*count=0;errno=saved;return 0;}
}
// Callers serialize installation. Only the verified SUB SP / STP prologues
// below may be replayed: no PC-relative instruction or branch relocation.
static void *mx_audio_detour(unsigned char *address,void *replacement,const uint32_t expected[4],void *original_slot) {
    uint32_t branch;
    uintptr_t target=(uintptr_t)address;
    const char *failure=NULL;int failure_errno=0,rollback_errno=0;
    if(target&3)failure="unaligned entry";
    else if(memcmp(address,expected,16))failure="16-byte guard mismatch";
    else if(expected[0]!=0xd10743ffu && expected[0]!=0xa9bd7bfdu &&
            expected[0]!=0xd10243ffu && expected[0]!=0xd10183ffu)
        failure="unsupported replay instruction";
    if(failure) {
        LOG("FrameBridge audio detour: target=%p failed: %s; entry unchanged",address,failure);
        return NULL;
    }
    if(!mx_audio_page || (mx_audio_page&(mx_audio_page-1))) {
        LOG("FrameBridge audio detour: target=%p failed: invalid page size=%zu; entry unchanged",address,mx_audio_page);
        return NULL;
    }
    struct mx_audio_candidate *candidates=NULL;size_t count=0;
    FILE *maps=fopen("/proc/self/maps","r");
    if(!maps) {
        LOG("FrameBridge audio detour: target=%p failed: open /proc/self/maps errno=%d; entry unchanged",address,errno);
        return NULL;
    }
    int parsed=mx_audio_candidates(maps,target,&candidates,&count,&failure);
    int parse_errno=errno;fclose(maps);
    if(!parsed) {
        LOG("FrameBridge audio detour: target=%p failed: %s errno=%d; entry unchanged",address,failure,parse_errno);
        return NULL;
    }
    unsigned char *code=MAP_FAILED;void *result=MAP_FAILED,*attempt=NULL;
    void *closest=count?(void*)candidates[0].address:NULL;
    int map_errno=0;size_t attempts=0;
    for(size_t i=0;i<count;i++) {
        attempt=(void*)candidates[i].address;attempts++;
        result=mmap(attempt,mx_audio_page,PROT_READ|PROT_WRITE,
                    MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
        if(result==MAP_FAILED) {map_errno=errno;continue;}
        // Old kernels may ignore NOREPLACE. Never accept a relocated mapping.
        if(result!=attempt) {munmap(result,mx_audio_page);map_errno=0;continue;}
        code=result;break;
    }
    free(candidates);
    if(code==MAP_FAILED) {
        LOG("FrameBridge audio detour: target=%p failed: stub allocation closest=%p attempted=%p mmap=%p errno=%d attempts=%zu; no safe ELF slack fallback; entry unchanged",
            address,closest,attempt,result,map_errno,attempts);
        return NULL;
    }
    uint32_t back;
    if(!mx_audio_branch(target,(uintptr_t)code,&branch) ||
       !mx_audio_branch((uintptr_t)code+20,target+4,&back)) {
        failure="branch range validation";goto fail;
    }
    mx_audio_jump(code,(uintptr_t)replacement);
    memcpy(code+16,expected,4);memcpy(code+20,&back,4);
    __builtin___clear_cache((char*)code,(char*)code+24);
    if(mprotect(code,mx_audio_page,PROT_READ|PROT_EXEC)) {failure="mprotect stub RX";failure_errno=errno;goto fail;}
    unsigned char *page=(void*)(target&~(mx_audio_page-1));
    if(mprotect(page,mx_audio_page,PROT_READ|PROT_WRITE|PROT_EXEC)) {failure="mprotect entry RWX";failure_errno=errno;goto fail;}
    // Publish the callable original before any worker can enter the hook.
    if(original_slot)__atomic_store_n((void**)original_slot,code+16,__ATOMIC_RELEASE);
    __atomic_store_n((uint32_t*)address,branch,__ATOMIC_RELEASE);
    __builtin___clear_cache((char*)address,(char*)address+4);
    if(mprotect(page,mx_audio_page,PROT_READ|PROT_EXEC)) {
        failure_errno=errno;
        __atomic_store_n((uint32_t*)address,expected[0],__ATOMIC_RELEASE);
        __builtin___clear_cache((char*)address,(char*)address+4);
        if(mprotect(page,mx_audio_page,PROT_READ|PROT_EXEC))
            rollback_errno=errno; // Cannot force the kernel to restore RX.
        // A worker may already be in the stub/hook/trampoline. No grace period
        // exists here, so neither the mapping nor original_slot can be retired.
        LOG("FrameBridge audio detour: target=%p failed: mprotect entry RX errno=%d; rollback mprotect entry RX errno=%d; entry restored; published code retained",address,failure_errno,rollback_errno);return NULL;
    }
    return code+16;
fail:
    munmap(code,mx_audio_page);
    LOG("FrameBridge audio detour: target=%p failed: %s errno=%d; entry unchanged",address,failure,failure_errno);return NULL;
}
