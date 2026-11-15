/* SPDX-License-Identifier: MIT; CEC.TXT rotation: fresh card, one previous, two previous, missing and empty files. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cec_rot.h"
static const char *const nm[3]={"CEC.TXT","CEC.1","CEC.2"};
struct fs {char body[3][16];int renames;};
static int idx(const char *n) {for(int i=0;i<3;i++) if(!strcmp(nm[i],n)) return i; assert(0);return -1;}
static bool has(const char *n,void *c) {struct fs *s=c;return s->body[idx(n)][0]!=0;}
static int rn(const char *f,const char *t,void *c) {
 struct fs *s=c;int a=idx(f),b=idx(t);s->renames++;
 if(!s->body[a][0]) return -2;
 strcpy(s->body[b],s->body[a]);s->body[a][0]=0;return 0;
}
static int run(struct fs *s) { /* a boot's rotation; returns the number of calls it took */
 struct cec_rot_io io={rn,has,s};unsigned step=0;int calls=0;
 while(1) {calls++;if(!cec_rot_step(&io,&step)) break;assert(calls<5);}
 return calls;
}
int main(void) {
 struct fs s={0};
 assert(run(&s)==1&&s.renames==0);                        /* fresh card: no CEC.TXT, nothing happens */
 strcpy(s.body[0],"b1");assert(run(&s)==2&&s.renames==1); /* one previous: CEC.TXT -> CEC.1 only */
 assert(!s.body[0][0]&&!strcmp(s.body[1],"b1")&&!s.body[2][0]);
 strcpy(s.body[0],"b2");run(&s);                          /* two previous */
 assert(!strcmp(s.body[1],"b2")&&!strcmp(s.body[2],"b1"));
 strcpy(s.body[0],"b3");run(&s);                          /* b1 ages out */
 assert(!strcmp(s.body[1],"b3")&&!strcmp(s.body[2],"b2"));
 s.renames=0;run(&s);                                     /* CEC.TXT absent (a boot that never saved): keep both */
 assert(s.renames==0&&!strcmp(s.body[1],"b3")&&!strcmp(s.body[2],"b2"));
 strcpy(s.body[0],"b4");s.body[1][0]=0;run(&s);           /* CEC.1 missing: CEC.2 untouched, CEC.TXT moves */
 assert(!strcmp(s.body[1],"b4")&&!strcmp(s.body[2],"b2"));
 puts("ok");return 0;
}
