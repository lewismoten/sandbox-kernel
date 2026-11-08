/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
#include "audio_core.h"
#include <string.h>
static void discard(struct audio_core *r) {
    memset(r->pcm,0,sizeof(r->pcm));
    r->head=r->queued=0;
}
static uint64_t sat_add(uint64_t v,uint64_t n) {
    return n>UINT64_MAX-v?UINT64_MAX:v+n;
}
int audio_core_stop(struct audio_core *r,uint64_t owner) {
    if (!owner || owner!=r->owner) return AUDIO_STALE;
    discard(r); return 0;
}
int audio_core_close(struct audio_core *r,uint64_t owner) {
    int rc=audio_core_stop(r,owner);
    if (!rc) r->owner=0;
    return rc;
}
void audio_core_fault(struct audio_core *r) {
    discard(r); r->owner=0; r->errors=sat_add(r->errors,1);
}
void audio_core_reset(struct audio_core *r) {
    uint64_t generation=r->generation;
    memset(r,0,sizeof(*r)); r->generation=generation;
}
void audio_core_init(struct audio_core *r) { memset(r,0,sizeof(*r)); }
int audio_core_open(struct audio_core *r,uint64_t *owner) {
    if (!owner) return AUDIO_INVALID;
    if (r->owner) return AUDIO_BUSY;
    if (r->generation==UINT64_MAX) return AUDIO_OVERFLOW;
    r->owner=++r->generation;
    r->head=r->queued=0;
    r->played=r->underruns=r->errors=0;
    *owner=r->owner;
    return 0;
}

int audio_core_write(struct audio_core *r,uint64_t owner,const void *span,size_t span_bytes,size_t offset,size_t bytes) {
 if (!owner || owner!=r->owner) return AUDIO_STALE;
 if (!bytes) return 0;
 if (!span || offset>span_bytes || bytes>span_bytes-offset ||
     offset>UINTPTR_MAX-(uintptr_t)span ||
     bytes>UINTPTR_MAX-((uintptr_t)span+offset)) return AUDIO_FAULT;
 if (bytes%AUDIO_FRAME_BYTES || bytes>AUDIO_MAX_WRITE*AUDIO_FRAME_BYTES ||
     ((uintptr_t)span+offset)%AUDIO_FRAME_BYTES) return AUDIO_INVALID;
 uint32_t frames=(uint32_t)(bytes/AUDIO_FRAME_BYTES);
 if (frames>AUDIO_FRAMES-r->queued) return AUDIO_AGAIN;
 const uint8_t *src=(const uint8_t *)span+offset;
 uint32_t tail=(r->head+r->queued)%AUDIO_FRAMES;
 for (uint32_t i=0;i<frames;i++)
     memcpy(r->pcm+((tail+i)%AUDIO_FRAMES)*4,src+i*4,4);
 r->queued+=frames;
 return 0;
}
int audio_core_drain(struct audio_core *r,void *out,size_t frames) {
 if (frames>AUDIO_MAX_WRITE || (frames && !out)) return AUDIO_INVALID;
 uint8_t *dst=out;
 size_t actual=frames<r->queued?frames:r->queued;
 for (size_t i=0;i<actual;i++) {
     memcpy(dst+i*4,r->pcm+r->head*4,4);
     r->head=(r->head+1)%AUDIO_FRAMES;
 }
 if (frames>actual) memset(dst+actual*4,0,(frames-actual)*4);
 r->queued-=(uint32_t)actual;
 r->played=sat_add(r->played,actual);
 if (frames>actual) r->underruns=sat_add(r->underruns,1);
 return 0;
}
