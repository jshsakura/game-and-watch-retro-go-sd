/* Platform seam supplied by Core/Src/porting/sm/main_sm.c on the device.
 * The existing sm_harness replaces main_sm.c with a host main and therefore
 * needs this explicit symbol after snes_catchupApu began calling apu_run(). */
void apu_run(void *apu, int cycles_to_run)
{
    (void)apu;
    (void)cycles_to_run;
}
