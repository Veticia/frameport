// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
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
static void mx_audio_detour_error(void) {
    static int logged;
    if(!__atomic_exchange_n(&logged,1,__ATOMIC_RELAXED))
        LOG("FrameBridge audio detour: installation failed; entry unchanged (published code retained on rollback)");
}
// Callers serialize installation. Only the verified SUB SP / STP prologues
// below may be replayed: no PC-relative instruction or branch relocation.
static void *mx_audio_detour(unsigned char *address,void *replacement,const uint32_t expected[4],void *original_slot) {
    uint32_t branch;
    uintptr_t target=(uintptr_t)address;
    if((target&3) || memcmp(address,expected,16) ||
       (expected[0]!=0xd10743ffu && expected[0]!=0xa9bd7bfdu &&
        expected[0]!=0xd10243ffu && expected[0]!=0xd10183ffu))return NULL;
    unsigned char *code=MAP_FAILED;
    // Hints never replace mappings. The kernel may ignore them: validate each
    // result, including the trampoline's direct branch back to entry+4.
    for(uintptr_t distance=mx_audio_page;distance<((uintptr_t)1<<27);distance+=0x100000) {
        for(unsigned side=0;side<2;side++) {
            uintptr_t base=target&~(mx_audio_page-1);
            if((side && base<distance) || (!side && base>UINTPTR_MAX-distance))continue;
            uintptr_t hint=side?base-distance:base+distance;
            code=mmap((void*)hint,mx_audio_page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
            if(code==MAP_FAILED)continue;
            uint32_t back;
            if(mx_audio_branch(target,(uintptr_t)code,&branch) &&
               mx_audio_branch((uintptr_t)code+20,target+4,&back)) {
                mx_audio_jump(code,(uintptr_t)replacement);
                memcpy(code+16,expected,4);memcpy(code+20,&back,4);
                goto allocated;
            }
            munmap(code,mx_audio_page);code=MAP_FAILED;
        }
    }
    mx_audio_detour_error();return NULL;
allocated:
    __builtin___clear_cache((char*)code,(char*)code+24);
    if(mprotect(code,mx_audio_page,PROT_READ|PROT_EXEC))goto fail;
    unsigned char *page=(void*)(target&~(mx_audio_page-1));
    if(mprotect(page,mx_audio_page,PROT_READ|PROT_WRITE|PROT_EXEC))goto fail;
    // Publish the callable original before any worker can enter the hook.
    if(original_slot)__atomic_store_n((void**)original_slot,code+16,__ATOMIC_RELEASE);
    __atomic_store_n((uint32_t*)address,branch,__ATOMIC_RELEASE);
    __builtin___clear_cache((char*)address,(char*)address+4);
    if(mprotect(page,mx_audio_page,PROT_READ|PROT_EXEC)) {
        __atomic_store_n((uint32_t*)address,expected[0],__ATOMIC_RELEASE);
        __builtin___clear_cache((char*)address,(char*)address+4);
        if(mprotect(page,mx_audio_page,PROT_READ|PROT_EXEC))
            mx_audio_detour_error(); // Cannot force the kernel to restore RX.
        // A worker may already be in the stub/hook/trampoline. No grace period
        // exists here, so neither the mapping nor original_slot can be retired.
        mx_audio_detour_error();return NULL;
    }
    return code+16;
fail:
    munmap(code,mx_audio_page);mx_audio_detour_error();return NULL;
}
