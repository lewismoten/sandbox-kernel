/* SPDX-License-Identifier: MIT; original freestanding helpers. */
#include "kernel.h"
void *memcpy(void *d,const void *s,size_t n) { uint8_t *a=d; const uint8_t *b=s; while(n--) *a++=*b++; return d; }
void *memset(void *d,int v,size_t n) { uint8_t *a=d; while(n--) *a++=(uint8_t)v; return d; }
int memcmp(const void *a,const void *b,size_t n) { const uint8_t *x=a,*y=b; while(n--) { if(*x!=*y) return *x-*y; x++; y++; } return 0; }
size_t strlen(const char *s) { size_t n=0; while(s[n]) n++; return n; }
void number(char *s,uint32_t n) { char a[10]; unsigned k=0; do { a[k++]=(char)('0'+n%10); n/=10; } while(n); while(k) *s++=a[--k]; *s=0; }
void uptime(char *s,uint32_t sec) {
 if(sec<100) { number(s,sec); s+=strlen(s); *s++='s'; *s=0; return; }
 const uint32_t u[]={31536000,604800,86400,3600,60}; const char names[]="ywdhm";
 unsigned i=0; while(i<4 && sec<u[i]) i++;
 number(s,sec/u[i]); s+=strlen(s); *s++=names[i]; sec%=u[i];
 if(i<4 && sec/u[i+1]) { *s++=' '; number(s,sec/u[i+1]); s+=strlen(s); *s++=names[i+1]; } *s=0;
}
uint32_t crc32(const void *p,size_t n) { const uint8_t *s=p; uint32_t c=~0u; while(n--) { c^=*s++; for(unsigned j=0;j<8;j++) c=(c>>1)^((0u-(c&1))&0xedb88320u); } return ~c; }
static uint32_t rotr(uint32_t x,unsigned n) { return (x>>n)|(x<<(32-n)); }
static const uint32_t K[64]={
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
void sha256(const void *p,size_t n,uint8_t out[32]) {
 const uint8_t *src=p; size_t original=n; uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
 size_t blocks=(n+9+63)/64;
 for(size_t b=0;b<blocks;b++) {
  uint32_t w[64]; uint8_t block[64];
  for(unsigned i=0;i<64;i++) { size_t at=b*64+i; block[i]=at<original?src[at]:(at==original?0x80:0); }
  if(b+1==blocks) { uint64_t bits=(uint64_t)original*8; for(unsigned i=0;i<8;i++) block[63-i]=(uint8_t)(bits>>(i*8)); }
  for(unsigned i=0;i<16;i++) w[i]=((uint32_t)block[i*4]<<24)|((uint32_t)block[i*4+1]<<16)|((uint32_t)block[i*4+2]<<8)|block[i*4+3];
  for(unsigned i=16;i<64;i++) w[i]=w[i-16]+(rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3))+w[i-7]+(rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10));
  uint32_t a=h[0],bb=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
  for(unsigned i=0;i<64;i++) { uint32_t t=hh+(rotr(e,6)^rotr(e,11)^rotr(e,25))+((e&f)^(~e&g))+K[i]+w[i]; uint32_t q=(rotr(a,2)^rotr(a,13)^rotr(a,22))+((a&bb)^(a&c)^(bb&c)); hh=g;g=f;f=e;e=d+t;d=c;c=bb;bb=a;a=t+q; }
  h[0]+=a;h[1]+=bb;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
 }
 for(unsigned i=0;i<32;i++) out[i]=(uint8_t)(h[i/4]>>(24-8*(i%4)));
}
bool image_valid(const uint8_t *p,uint32_t n) {
 if(n<260 || n>MAX_IMAGE) return false;
 uint32_t w[8]; memcpy(w,p,32);
 if(w[0]!=IMAGE_MAGIC || w[1]!=1 || w[2]!=PROFILE || w[3]!=0x10000 || w[4]!=n-256 || w[5]!=RUNTIME_BASE || w[6]!=RUNTIME_BASE || w[7]) return false;
 bool terminated=false;
 for(unsigned i=64;i<96;i++) {if(!p[i])terminated=true;else if(terminated||p[i]<32||p[i]>126)return false;}
 if(!terminated)return false;
 for(unsigned i=96;i<256;i++) if(p[i]) return false;
 uint8_t hash[32]; sha256(p+256,n-256,hash); return !memcmp(hash,p+32,32);
}

int strcmp(const char *a,const char *b) {while(*a&&*a==*b){a++;b++;}return (unsigned char)*a-(unsigned char)*b;}
