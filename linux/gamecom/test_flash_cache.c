/* Regression test: reproduce the DEVICE circular flash cache (gw_flash_alloc.c
 * circular_flash_write) in the host harness, proving the BIOS-clobber freeze and
 * that the cart-first / BIOS-last order in main_gamecom.c prevents it — without
 * flashing. Run:  make -f Makefile.gamecom test-cache
 *
 * Needs the real Tiger BIOS (internal.bin/external.bin) + one cart on disk; pass
 * paths as argv or drop them in linux/. Exit 0 = fix verified, non-zero = broken. */
#include "gamecom_core.h"
#include "sm8500.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern uint8_t gamecom_fb[];
static unsigned char* slurp(const char*p,int*n){FILE*f=fopen(p,"rb");if(!f){*n=0;return 0;}fseek(f,0,2);long s=ftell(f);fseek(f,0,0);unsigned char*b=malloc(s);if(fread(b,1,s,f)!=(size_t)s){};fclose(f);*n=s;return b;}

/* --- circular flash cache sim (mirrors device circular_flash_write) --- */
#define BLK 4096
static uint8_t *region; static uint32_t REGION, wptr, nfiles;
typedef struct{uint32_t addr,size;int valid;const void*tag;} CF; static CF files[64];
static uint32_t blkup(uint32_t x){return (x+BLK-1)&~(BLK-1);}
static const uint8_t* cache_file(const uint8_t*data,uint32_t size,const void*tag){
  for(uint32_t i=0;i<nfiles;i++) if(files[i].valid&&files[i].tag==tag&&files[i].size==size) return region+files[i].addr;
  uint32_t asize=blkup(size);
  if(wptr+asize>REGION) wptr=0;
  if(wptr+asize>REGION) return NULL;
  uint32_t at=wptr,end=at+asize;
  for(uint32_t i=0;i<nfiles;i++) if(files[i].valid){uint32_t fs=files[i].addr,fe=fs+files[i].size; if(at<fe&&fs<end) files[i].valid=0;}
  memcpy(region+at,data,size); files[nfiles++]=(CF){at,size,1,tag}; wptr+=asize; return region+at;
}
/* returns 1 if it boots (renders), 0 if frozen */
static int run_order(const uint8_t*I,int il,const uint8_t*K,int kl,const uint8_t*CA,int cl,
                     int cart_first,uint32_t region_kb,uint32_t start_kb){
  REGION=region_kb*1024; wptr=start_kb*1024; nfiles=0;
  free(region); region=malloc(REGION); memset(region,0xEE,REGION);
  const uint8_t*ib,*eb,*cb;
  if(cart_first){ cb=cache_file(CA,cl,CA); ib=cache_file(I,il,I); eb=cache_file(K,kl,K); }
  else          { ib=cache_file(I,il,I); eb=cache_file(K,kl,K); cb=cache_file(CA,cl,CA); }
  if(!ib||!eb) return 0;
  gamecom_init(ib,il,eb,kl,cb,cl); gamecom_set_input_state(0xFF,0xFF,0xFF);
  int firstnb=-1,peak=0;
  for(int f=0;f<700;f++){int d=(f>=400&&f<520); gamecom_set_stylus(45,60,d); gamecom_run_frame();
    int nb=0; for(int p=0;p<200*160;p++) if(gamecom_fb[p])nb++; if(nb>peak)peak=nb; if(firstnb<0&&nb>0)firstnb=f;}
  return (firstnb>=0 && peak>5000);
}
int main(int c,char**v){
  const char*ip=c>1?v[1]:"internal.bin",*ep=c>2?v[2]:"external.bin",*cp=c>3?v[3]:"sonicjam.bin";
  int il,kl,cl; const uint8_t*I=slurp(ip,&il),*K=slurp(ep,&kl),*CA=slurp(cp,&cl);
  if(!I||!K||!CA||il<0x1000||kl<0x40000){ printf("SKIP: need %s + %s + a 2MB cart %s\n",ip,ep,cp); return 77; }
  /* the device-clobbering scenario: 2MB cart, region just above cart, mid-flash pointer */
  int old_boots = run_order(I,il,K,kl,CA,cl,0,2560,410);  /* BIOS-first  (the bug) */
  int new_boots = run_order(I,il,K,kl,CA,cl,1,2560,410);  /* cart-first  (the fix) */
  printf("flash-cache regression: OLD(bios-first)=%s  NEW(cart-first)=%s\n",
         old_boots?"boots":"FROZEN", new_boots?"boots":"FROZEN");
  if(!new_boots){ printf("FAIL: cart-first order does NOT survive the flash cache\n"); return 1; }
  if(old_boots){ printf("WARN: bios-first no longer clobbers here (scenario stale) — fix still OK\n"); return 0; }
  printf("PASS: reproduced the bios-first freeze AND verified cart-first prevents it.\n");
  return 0;
}
