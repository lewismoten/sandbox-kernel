/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
#include "audio_core.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    struct audio_core r;
    uint64_t owner=0, second=99;
    audio_core_init(&r);
    assert(audio_core_open(&r,&owner)==0 && owner!=0);
    assert(audio_core_open(&r,&second)==AUDIO_BUSY && second==99);
    puts("PASS exclusive generation-bound owner");
    _Alignas(4) uint8_t in[7680], out[7680];
    for (size_t i=0;i<sizeof(in);i++) in[i]=(uint8_t)(i*17u);
    assert(audio_core_write(&r,owner,in,sizeof(in),0,sizeof(in))==0);
    assert(r.queued==1920);
    memset(in,0,sizeof(in)); /* never retain the application buffer */
    assert(audio_core_drain(&r,out,1920)==0 && r.queued==0);
    for (size_t i=0;i<sizeof(out);i++) assert(out[i]==(uint8_t)(i*17u));
    puts("PASS copied stereo LE bytes, empty/full drain");
    /* Atomic capacity and validation rejection witnesses: exact whole-state snapshot. */
    for (unsigned i=0;i<2;i++) assert(audio_core_write(&r,owner,in,sizeof(in),0,sizeof(in))==0);
    assert(audio_core_write(&r,owner,in,sizeof(in),0,256*4)==0 && r.queued==4096);
    struct audio_core before=r;
    assert(audio_core_write(&r,owner,in,sizeof(in),0,4)==AUDIO_AGAIN);
    assert(memcmp(&r,&before,sizeof(r))==0);
    assert(audio_core_write(&r,owner,NULL,0,SIZE_MAX,0)==0);
    assert(audio_core_write(&r,owner,in,sizeof(in),0,7684)==AUDIO_FAULT);
    assert(audio_core_write(&r,owner,in,sizeof(in),SIZE_MAX,4)==AUDIO_FAULT);
    assert(audio_core_write(&r,owner,in,sizeof(in),1,4)==AUDIO_INVALID);
    assert(audio_core_write(&r,owner,in,sizeof(in),0,3)==AUDIO_INVALID);
    assert(audio_core_write(&r,owner+1,in,sizeof(in),0,4)==AUDIO_STALE);
    assert(memcmp(&r,&before,sizeof(r))==0);
    assert(audio_core_drain(&r,out,1920)==0);
    assert(audio_core_drain(&r,out,1920)==0);
    assert(audio_core_drain(&r,out,256)==0 && !r.queued);
    puts("PASS wrap, capacity and rejection atomicity");
    assert(audio_core_stop(&r,owner)==0);
    assert(audio_core_close(&r,owner)==0);
    assert(audio_core_open(&r,&second)==0 && second!=owner);
    assert(audio_core_write(&r,owner,in,sizeof(in),0,4)==AUDIO_STALE);
    audio_core_fault(&r);
    assert(!r.owner && !r.queued && r.errors==1);
    assert(audio_core_open(&r,&owner)==0);
    audio_core_reset(&r);
    assert(!r.owner && !r.queued);
    assert(audio_core_open(&r,&second)==0 && second!=owner);
    puts("PASS stop/close/fault/reset revoke old ownership");
    assert(audio_core_write(&r,second,in,sizeof(in),0,8)==0);
    r.played=UINT64_MAX-1; r.underruns=UINT64_MAX;
    assert(audio_core_drain(&r,out,3)==0);
    assert(r.played==UINT64_MAX && r.underruns==UINT64_MAX);
    assert(!out[8] && !out[9] && !out[10] && !out[11]);
    r.errors=UINT64_MAX; audio_core_fault(&r); assert(r.errors==UINT64_MAX);
    r.generation=UINT64_MAX;
    assert(audio_core_open(&r,&owner)==AUDIO_OVERFLOW);
    puts("PASS underrun silence and saturating counters/generation exhaustion");
    audio_core_init(&r); assert(audio_core_open(&r,&owner)==0);
    assert(audio_core_write(&r,owner,(void *)(UINTPTR_MAX-2),8,4,4)==AUDIO_FAULT);
    puts("PASS address-addition overflow denied before dereference");
    _Alignas(4) uint8_t large[7684]={0};
    before=r;
    assert(audio_core_write(&r,owner,large,sizeof(large),0,sizeof(large))==AUDIO_INVALID);
    assert(audio_core_write(&r,owner,NULL,4,0,4)==AUDIO_FAULT);
    assert(audio_core_close(&r,owner+1)==AUDIO_STALE);
    assert(audio_core_stop(&r,0)==AUDIO_STALE);
    assert(audio_core_drain(&r,out,1921)==AUDIO_INVALID);
    assert(!memcmp(&before,&r,sizeof(r)));
    uint8_t model[16384]; unsigned used=0; uint32_t rng=0x50434d31;
    for (unsigned op=0;op<20000;op++) {
        rng=rng*1664525u+1013904223u;
        unsigned frames=(rng>>8)%1921u;
        if(rng&1) {
            for(unsigned i=0;i<frames*4;i++) in[i]=(uint8_t)(rng+i);
            before=r;
            int rc=audio_core_write(&r,owner,in,sizeof(in),0,frames*4);
            if(frames<=4096-used) {
                assert(rc==0);memcpy(model+used*4,in,frames*4);used+=frames;
            } else {assert(rc==AUDIO_AGAIN);assert(!memcmp(&before,&r,sizeof(r)));}
        } else {
            unsigned actual=frames<used?frames:used;
            memset(out,0x55,sizeof(out));
            assert(audio_core_drain(&r,out,frames)==0);
            assert(!memcmp(out,model,actual*4));
            for(unsigned i=actual*4;i<frames*4;i++) assert(out[i]==0);
            memmove(model,model+actual*4,(used-actual)*4);used-=actual;
        }
        assert(r.queued==used && r.head<4096);
    }
    puts("PASS 20000 deterministic independent FIFO/model transitions");
    return 0;
}
