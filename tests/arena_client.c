/* SPDX-License-Identifier: MIT; optional synthetic NON-AUDIO SDK client.
 * Built by qualification only, never installed or made the default program. */
#include <sandbox/syscall.h>
int main(const sbx_startup *startup) {
 void *address=0;uint64_t actual=0;
 int e=sbx_app_arena(startup,1,&address,&actual);
 if(e==-SBX_ENOSYS) return 0; /* old kernel / skew: feature disabled */
 if(e) return 1;
 if((uintptr_t)address!=SBX_APP_ARENA_ADDRESS||actual!=SBX_APP_ARENA_BYTES) return 2;
 volatile unsigned char *p=address;
 if(p[0]||p[actual-1]) return 3;
 p[0]=0x5a;p[actual-1]=0xa5;
 if(sbx_app_arena(startup,actual,&address,&actual)||p[0]!=0x5a||p[actual-1]!=0xa5) return 4;
 return 0;
}
