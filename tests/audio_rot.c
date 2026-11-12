/* SPDX-License-Identifier: MIT; AUDIO.TXT rotation and the no-display link boot rule. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "audio_rot.h"
struct fs {char name[3][12];char body[3][16];};
static int rn(const char *f,const char *t,void *c) {
 struct fs *s=c;int a=-1,b=-1;
 for(int i=0;i<3;i++) {char n[12];strcpy(n,i?(i==1?"AUDIO.1":"AUDIO.2"):"AUDIO.TXT");if(!strcmp(n,f))a=i;if(!strcmp(n,t))b=i;}
 if(a<0||b<0||!s->body[a][0]) return -2;
 strcpy(s->body[b],s->body[a]);s->body[a][0]=0;return 0;
}
int main(void) {
 struct fs s={0};struct audio_rot_io io={rn,&s};
 strcpy(s.body[0],"boot1");assert(audio_rot_rotate(&io)==0);assert(!strcmp(s.body[1],"boot1")&&!s.body[0][0]);
 strcpy(s.body[0],"boot2");audio_rot_rotate(&io);
 strcpy(s.body[0],"boot3");audio_rot_rotate(&io);
 assert(!strcmp(s.body[1],"boot3")&&!strcmp(s.body[2],"boot2")); /* boot1 aged out */
 assert(audio_rot_is_link("n5,link,k0")&&!audio_rot_is_link("n5,keyboard,k1")&&!audio_rot_is_link("")&&!audio_rot_is_link(0));
 assert(!audio_rot_should_write("n5,link,k0",false)); /* link boot, no display: keep the previous record */
 assert(audio_rot_should_write("n5,link,k0",true));
 assert(audio_rot_should_write("n5,keyboard,k1",false)&&audio_rot_should_write("",false));
 char h[80];unsigned n=audio_rot_header(h,sizeof h,"n30,keyboard,k2",7,3);
 assert(n==strlen(h)&&!strcmp(h,"AUDIO v2 boot=30 mode=keyboard gen=7 dropped_lines=3"));
 audio_rot_header(h,sizeof h,"",0,0);assert(!strcmp(h,"AUDIO v2 boot=? mode=unknown gen=0 dropped_lines=0"));
 audio_rot_header(h,12,"n30,link,k0",7,3);assert(strlen(h)==11);
 puts("ok");return 0;
}
