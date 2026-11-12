/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
/* Keep the audio records of the last three boots; see include/audio_rot.h. */
#include "audio_rot.h"
static bool same(const char *a,const char *b) {while(*a&&*a==*b) {a++;b++;}return *a==*b;}
static bool decision(const char *r,char *o,unsigned cap) { /* the text between the first two commas */
 const char *c=r;while(c&&*c&&*c!=',') c++;if(!c||!*c) return false;c++;
 unsigned n=0;while(c[n]&&c[n]!=','&&c[n]>' '&&n+1<cap) {o[n]=c[n];n++;}
 o[n]=0;return n>0;
}
bool audio_rot_is_link(const char *r) {char d[16];return decision(r,d,sizeof d)&&same(d,"link");}
bool audio_rot_should_write(const char *r,bool display_seen) {return !audio_rot_is_link(r)||display_seen;}
int audio_rot_rotate(const struct audio_rot_io *io) {
 io->rename("AUDIO.1","AUDIO.2",io->ctx);
 return io->rename("AUDIO.TXT","AUDIO.1",io->ctx);
}
static unsigned put(char *o,unsigned at,unsigned cap,const char *s) {while(*s&&at+1<cap) o[at++]=*s++;return at;}
static unsigned num(char *o,unsigned at,unsigned cap,uint32_t v) {
 char d[11];unsigned k=0;do {d[k++]=(char)('0'+v%10);v/=10;} while(v);
 while(k) {if(at+1<cap) o[at++]=d[--k];else k--;}
 return at;
}
unsigned audio_rot_header(char *o,unsigned cap,const char *r,uint32_t gen,uint32_t dropped) {
 unsigned at=put(o,0,cap,"AUDIO v2 boot=");
 if(r&&r[0]=='n'&&r[1]>='0'&&r[1]<='9') {uint32_t v=0;for(unsigned i=1;r[i]>='0'&&r[i]<='9';i++) v=v*10+(uint32_t)(r[i]-'0');at=num(o,at,cap,v);}
 else at=put(o,at,cap,"?");
 char d[16];at=put(o,at,cap," mode=");at=put(o,at,cap,decision(r,d,sizeof d)?d:"unknown");
 at=put(o,at,cap," gen=");at=num(o,at,cap,gen);
 at=put(o,at,cap," dropped_lines=");at=num(o,at,cap,dropped);
 o[at]=0;return at;
}
