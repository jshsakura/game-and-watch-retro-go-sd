/* Read-only observers: no extra guest reads, no scheduling changes. */
#include <stdio.h>
#define MEM_SLOTS 1024
struct mem_site {
  uint32_t pc, op, core, write, width, page, lo, hi;
  uint64_t count;
};
static struct mem_site mem_sites[MEM_SLOTS];
static uint64_t mem_dropped, poll_reads, poll_zero, poll_nonzero;
static uint32_t poll_pc[2], poll_addr[2], poll_lo=0xffffffffu, poll_hi;
static uint32_t poll_r2_lo=0xffffffffu, poll_r2_hi;
static uint32_t poll_values[16], poll_value_n;
static uint64_t poll_value_counts[16], poll_value_dropped;
struct flag_store {uint32_t core,pc,a,width,value;uint64_t count;};
static struct flag_store flag_stores[64];
static uint64_t flag_store_dropped;
static void flag_pre(SH2 *s,uint16_t op) {
  unsigned n=(op>>8)&15,m=(op>>4)&15,low=op&15,major=op>>12,w=0;
  uint32_t a=0,d=0;
  if(major==2 && (low<=2 || (low>=4 && low<=6))) {
    w=1u<<(low&3);a=s->r[n]-(low>=4?w:0);d=s->r[m];
  } else if(major==0 && low>=4 && low<=6) {w=1u<<(low-4);a=s->r[0]+s->r[n];d=s->r[m];
  } else if(major==1) {w=4;a=s->r[n]+low*4;d=s->r[m];
  } else if((op>>8)==0x80 || (op>>8)==0x81) {w=(op>>8)==0x80?1:2;a=s->r[m]+low*w;d=s->r[0];
  } else if(major==12 && n<=2) {w=1u<<n;a=s->gbr+(op&255)*w;d=s->r[0];}
  a &= ~0x20000000u;
  if(!w || a>0x06003af1u || a+w<=0x06003af1u)return;
  uint32_t value=(d>>((w-1-(0x06003af1u-a))*8))&255;
  unsigned c=s->is_slave&1;
  for(unsigned i=0;i<64;i++) {
    struct flag_store *e=&flag_stores[i];
    if(!e->count){e->core=c;e->pc=s->ppc;e->a=a;e->width=w;e->value=value;}
    if(e->core==c && e->pc==s->ppc && e->a==a && e->width==w && e->value==value){e->count++;return;}
  }
  flag_store_dropped++;
}
static void mem_pre(SH2 *s, uint16_t op) {
  unsigned c=s->is_slave&1, pc=s->ppc, a=0, width=0, write=0;
  poll_pc[c]=0;
  if(rig_frame_no<TRACE_FROM || rig_frame_no>=TRACE_TO) return;
  flag_pre(s,op);
  if(pc==0x060038eeu && op==0x8411) {
    poll_pc[c]=pc; poll_addr[c]=s->r[1]+1;
    if(s->r[2]<poll_r2_lo) poll_r2_lo=s->r[2];
    if(s->r[2]>poll_r2_hi) poll_r2_hi=s->r[2];
  }
  if(!(pc>=0x06006800u && pc<0x06006b00u) && !poll_pc[c]) return;
  unsigned n=(op>>8)&15,m=(op>>4)&15,low=op&15,major=op>>12;
  if(major==2 && (low<=2 || (low>=4 && low<=6))) {
    width=1u<<(low&3); write=1; a=s->r[n]-(low>=4?width:0);
  } else if(major==6 && (low<=2 || (low>=4 && low<=6))) {
    width=1u<<(low&3); a=s->r[m];
  } else if(major==0 && low>=12 && low<=14) {
    width=1u<<(low-12); a=s->r[0]+s->r[m];
  } else if(major==0 && low>=4 && low<=6) {
    width=1u<<(low-4); a=s->r[0]+s->r[n]; write=1;
  } else if((op>>8)==0x84 || (op>>8)==0x85) {
    width=(op>>8)==0x84?1:2; a=s->r[m]+low*width;
  } else if((op>>8)==0x80 || (op>>8)==0x81) {
    width=(op>>8)==0x80?1:2; a=s->r[m]+low*width;write=1;
  } else if(major==5) { width=4;a=s->r[m]+low*4;
  } else if(major==1) { width=4;a=s->r[n]+low*4;write=1; }
  if(!width) return;
  unsigned page=a>>16, slot=((pc>>1)^(page*2654435761u)^c)&(MEM_SLOTS-1);
  for(unsigned i=0;i<32;i++,slot=(slot+1)&(MEM_SLOTS-1)) {
    struct mem_site *e=&mem_sites[slot];
    if(!e->count) {e->pc=pc;e->op=op;e->core=c;e->page=page;e->write=write;e->width=width;e->lo=e->hi=a;}
    if(e->pc==pc && e->op==op && e->core==c && e->page==page) {
      e->count++;if(a<e->lo)e->lo=a;if(a>e->hi)e->hi=a;return;
    }
  }
  mem_dropped++;
}
static void mem_post(SH2 *s) {
  unsigned c=s->is_slave&1;
  if(!poll_pc[c]) return;
  uint32_t a=poll_addr[c],v=s->r[0];
  poll_reads++;if(v)poll_nonzero++;else poll_zero++;
  if(a<poll_lo)poll_lo=a;if(a>poll_hi)poll_hi=a;
  for(unsigned i=0;i<poll_value_n;i++) if(poll_values[i]==v){poll_value_counts[i]++;return;}
  if(poll_value_n<16){poll_values[poll_value_n]=v;poll_value_counts[poll_value_n++]=1;}
  else poll_value_dropped++;
}
static void mem_report(void) {
  printf("MEM_DROPPED %llu\n",(unsigned long long)mem_dropped);
  for(unsigned i=0;i<MEM_SLOTS;i++) {
    struct mem_site *e=&mem_sites[i];if(!e->count)continue;
    printf("MEM_SITE %u %08lx %04lx %u %u %08lx %08lx %llu\n",e->core,
      (unsigned long)e->pc,(unsigned long)e->op,e->write,e->width,
      (unsigned long)e->lo,(unsigned long)e->hi,(unsigned long long)e->count);
  }
  printf("POLL %08lx %08lx %llu %llu %llu %lu %lu\n",(unsigned long)poll_lo,
    (unsigned long)poll_hi,(unsigned long long)poll_reads,(unsigned long long)poll_zero,
    (unsigned long long)poll_nonzero,(unsigned long)poll_r2_lo,(unsigned long)poll_r2_hi);
  for(unsigned i=0;i<poll_value_n;i++)printf("POLL_VALUE %08lx %llu\n",
    (unsigned long)poll_values[i],(unsigned long long)poll_value_counts[i]);
  printf("POLL_VALUE_DROPPED %llu\n",(unsigned long long)poll_value_dropped);
  for(unsigned i=0;i<64;i++){
    struct flag_store *e=&flag_stores[i];if(!e->count)continue;
    printf("FLAG_STORE %u %08lx %08lx %u %lu %llu\n",e->core,(unsigned long)e->pc,
      (unsigned long)e->a,e->width,(unsigned long)e->value,(unsigned long long)e->count);
  }
  printf("FLAG_STORE_DROPPED %llu\n",(unsigned long long)flag_store_dropped);
  printf("MEM_END\n");
}
