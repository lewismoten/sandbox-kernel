/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * Original host-only PCM core: Hermes GPT-6.1 Sol / openai-codex.
 * NOT an SDK/ABI or an available device. Unlinked; serialized callers only.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#define AUDIO_RATE 48000u
#define AUDIO_FRAMES 4096u
#define AUDIO_MAX_WRITE 1920u
#define AUDIO_FRAME_BYTES 4u
enum { AUDIO_INVALID=-22, AUDIO_AGAIN=-11, AUDIO_FAULT=-14,
       AUDIO_BUSY=-16, AUDIO_STALE=-9, AUDIO_OVERFLOW=-75 };
struct audio_core {
    uint8_t pcm[AUDIO_FRAMES*AUDIO_FRAME_BYTES];
    uint32_t head,queued;
    uint64_t generation,owner,played,underruns,errors;
};
/* Initialize once on a new object; reset must preserve generation history. */
void audio_core_init(struct audio_core *r);
int audio_core_open(struct audio_core *r,uint64_t *owner);
/* Trusted mapped-span seam, NOT a user-pointer validator. Future syscall must
 * check full mapped permissions and pin/copy under its own lifetime lock first.
 * span cannot alias this object's private memory. No pointer is retained.
 * All calls (including drain) must be serialized by eventual integration.
 */
int audio_core_write(struct audio_core *r,uint64_t owner,const void *span,
                     size_t span_bytes,size_t offset,size_t bytes);
/* Kernel-private output, <=1920 frames. Missing frames are zero, no stale replay. */
int audio_core_drain(struct audio_core *r,void *out,size_t frames);
int audio_core_stop(struct audio_core *r,uint64_t owner);
int audio_core_close(struct audio_core *r,uint64_t owner);
void audio_core_fault(struct audio_core *r);
void audio_core_reset(struct audio_core *r);
