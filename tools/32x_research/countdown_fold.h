/* Experimental generic SDRAM flag/countdown fold. Offline proof only. */
static unsigned long long countdown_iterations;
static unsigned countdown_calls;
static unsigned countdown_fold(SH2 *s) {
  if(s->delay || s->test_irq || s->icount<15 || !s->p_sdram) return 0;
  uint32_t pc=s->pc;
  if((pc&0xdf000000u)!=0x06000000u || (pc&0x3ffffu)>0x3fff6u || (pc&1))return 0;
  const uint16_t *code=(const uint16_t *)((const uint8_t *)s->p_sdram+(pc&0x3fffeu));
  uint16_t load=code[0],dt=code[3];
  if((load&0xff00)!=0x8400 || code[1]!=0x2008 ||
     (code[2]&0xff80)!=0x8b00 || (dt&0xf0ff)!=0x4010 || code[4]!=0x8bfa)return 0;
  unsigned base=(load>>4)&15,counter=(dt>>8)&15;
  if(!base || !counter || counter==base)return 0;
  uint32_t a=s->r[base]+(load&15),n=s->r[counter];
  if((a&0xdf000000u)!=0x06000000u || n<2)return 0;
  if(((const uint8_t *)s->p_sdram)[(a&0x3ffffu)^1])return 0;
  unsigned k=(s->icount-1)/7;
  if(k>=n)k=n-1;
  if(!k)return 0;
  s->r[0]=0;s->r[counter]=n-k;s->t_flag=0;
  s->pc=pc;s->ppc=pc+8;s->ea=pc;
  s->no_polling=SH2_NO_POLLING;s->icount-=7*k;
  countdown_iterations+=k;countdown_calls++;
  return k;
}
