// SPDX-License-Identifier: GPL-3.0-only
// Meta XR Audio Wwise metadata retirement. Enabled automatically for the
// exact verified AArch64 SDK build; unrelated libraries remain untouched.
#pragma once
#if defined(__aarch64__)
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <link.h>
#include <string.h>
#include "audio_detour.h"
#include "audio_metadata_queue.h"
typedef struct {float previous,current;} MxAudioRamp;
static uintptr_t mx_audio_base;
// set once the library was found but isn't the verified build: stop looking (xrPollEvent runs several times a frame)
static int mx_audio_checked;
static uint64_t (*mx_audio_consume_original)(void*,void*,void*,const void*,MxAudioRamp);
static void mx_audio_kill(unsigned experimental) {
    void *mutex=(void*)(mx_audio_base+(experimental?0x5fe2e0:0x5fe380));
    int (*trylock)(void*)=(void*)(mx_audio_base+0x5b369c);
    void (*unlock)(void*)=(void*)(mx_audio_base+0x5b36bc);
    if(!trylock(mutex))return;
    mx_audio_drain((void**)(mx_audio_base+(experimental?0x5fe2d8:0x5fe378)),experimental?40:24);
    unlock(mutex);
}
static void mx_audio_kill_experimental(void) {mx_audio_kill(1);}
static void mx_audio_kill_regular(void) {mx_audio_kill(0);}
static uint64_t mx_audio_consume(void *sink,void *main_mix,void *passthrough,const void *objects,MxAudioRamp ramp) {
    const unsigned char *previous=mx_audio_objects;mx_audio_objects=objects;
    // The installer publishes the completed trampoline before the entry branch.
    // Keep the acquire load paired with that publication.
    uint64_t (*original)(void*,void*,void*,const void*,MxAudioRamp);
    while(!(original=__atomic_load_n(&mx_audio_consume_original,__ATOMIC_ACQUIRE)))sched_yield();
    uint64_t result=original(sink,main_mix,passthrough,objects,ramp);
    mx_audio_objects=previous;return result;
}
static int mx_audio_find(struct dl_phdr_info *info,size_t size,void *unused) {
    (void)size;(void)unused;
    if(!strstr(info->dlpi_name,"/libMetaXRAudioWwise.so"))return 0;
    if(mx_audio_base)return 1;
    const unsigned char id[20]={0xe1,0x61,0x9e,0x7f,0xb8,0x39,0xba,0xdf,0x0e,0xa5,0xec,0xc2,0x2d,0x9f,0x02,0x8d,0x0b,0x17,0xc3,0x4d};
    int match=0;
    for(unsigned i=0;i<info->dlpi_phnum;i++)if(info->dlpi_phdr[i].p_type==PT_NOTE) {
        const unsigned char *p=(void*)(info->dlpi_addr+info->dlpi_phdr[i].p_vaddr);
        const unsigned char *end=p+info->dlpi_phdr[i].p_memsz;
        while(p+sizeof(Elf64_Nhdr)<=end) {
            const Elf64_Nhdr *note=(void*)p;
            const unsigned char *name=p+sizeof(*note),*desc=name+((note->n_namesz+3)&~3u);
            const unsigned char *next=desc+((note->n_descsz+3)&~3u);
            if(next>end || next<=p)break;
            if(note->n_type==NT_GNU_BUILD_ID && note->n_namesz==4 && !memcmp(name,"GNU",4) && note->n_descsz==20 && !memcmp(desc,id,20))match=1;
            p=next;
        }
    }
    if(!match){__atomic_store_n(&mx_audio_checked,1,__ATOMIC_RELEASE);return 1;}
    const uint32_t consume[4]={0xd10743ff,0x6d1623e9,0xa9177bfd,0xa9186ffc};
    const uint32_t kill[4]={0xa9bd7bfd,0xf9000bf5,0xa9024ff4,0x910003fd};
    unsigned char *base=(void*)info->dlpi_addr;
    if(memcmp(base+0x2c8fac,consume,16) || memcmp(base+0x2be2c0,kill,16) || memcmp(base+0x2be7b0,kill,16)) {
        __atomic_store_n(&mx_audio_checked,1,__ATOMIC_RELEASE);return 1;
    }
    __atomic_store_n(&mx_audio_base,info->dlpi_addr,__ATOMIC_RELEASE);
    void *original=mx_audio_detour(base+0x2c8fac,(void*)mx_audio_consume,consume,&mx_audio_consume_original);
    if(!original) {
        __atomic_store_n(&mx_audio_base,0,__ATOMIC_RELEASE);return 1;
    }
    if(!mx_audio_detour(base+0x2be2c0,(void*)mx_audio_kill_experimental,kill,NULL) || !mx_audio_detour(base+0x2be7b0,(void*)mx_audio_kill_regular,kill,NULL)) {
        LOG("FrameBridge audio metadata: incomplete installation");return 1;
    }
    LOG("FrameBridge audio metadata: metadata reclamation follows current audio-frame references; Meta audio base=%p",base);
    return 1;
}
static pthread_mutex_t mx_audio_install_mutex=PTHREAD_MUTEX_INITIALIZER;
static void mx_audio_initialize(void) {
    if(__atomic_load_n(&mx_audio_base,__ATOMIC_ACQUIRE) || __atomic_load_n(&mx_audio_checked,__ATOMIC_ACQUIRE))return;
    // until the game loads the library: look at most once a second (dl_iterate_phdr takes the linker's lock)
    static _Atomic long mx_audio_last_scan;
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
    if(now.tv_sec==mx_audio_last_scan)return;
    mx_audio_last_scan=now.tv_sec;
    if(pthread_mutex_trylock(&mx_audio_install_mutex))return;
    if(!mx_audio_page)mx_audio_page=(size_t)sysconf(_SC_PAGESIZE);
    if(!mx_audio_base)dl_iterate_phdr(mx_audio_find,NULL);
    pthread_mutex_unlock(&mx_audio_install_mutex);
}
#else
static void mx_audio_initialize(void) {}
#endif
