/* SPDX-License-Identifier: MIT; exercise production slot state at lifetime boundaries. */
#include "../core/sys.c"
#include <assert.h>
#include <stdio.h>
int main(void) {
 uint64_t old,fd;
 assert(!open_path("/dev/log",8,1,&old));release(0);
 for(unsigned n=0;n<65534;n++) {assert(!open_path("/dev/log",8,1,&fd));release(0);}
 assert(!open_path("/dev/log",8,1,&fd));
 assert(fd!=old&&lookup(old)==-EBADF&&lookup(fd)==0);
 assert(fd>>24); /* all generation bits participate in lookup */
 sys_reset();assert(lookup(fd)==-EBADF);
 fds[0].gen=HANDLE_GEN_MAX;
 assert(!open_path("/dev/log",8,1,&fd));assert(lookup(fd)==0);
 release(0);assert(lookup(fd)==-EBADF);
 uint64_t next;assert(!open_path("/dev/log",8,1,&next));assert((next&255)==1);
 sys_reset();sys_reset();assert(fds[0].gen==HANDLE_GEN_MAX+1);
 assert(!open_path("/dev/log",8,1,&next));assert((next&255)==1);
 sys_reset();
 for(unsigned i=0;i<SYS_HANDLES;i++) fds[i].gen=HANDLE_GEN_MAX+1;
 assert(open_path("/dev/log",8,1,&next)==-EMFILE);
 sys_reset();assert(open_path("/dev/log",8,1,&next)==-EMFILE);
 puts("handle generation lifetime ok");
}
