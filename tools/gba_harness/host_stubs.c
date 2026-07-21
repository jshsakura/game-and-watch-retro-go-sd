/* The last few symbols the host link needs that gba_frontend.c does not define.
 *
 * gba_frontend.c is the device's real front-end and is linked here unchanged —
 * that is the whole point of this harness. But the firmware is built -O2 and the
 * host at -O1, and at -O1 the compiler keeps a call the firmware's optimiser
 * folds away, so one more symbol becomes live. It is stubbed rather than added to
 * gba_frontend.c: the device does not need it, and dead code in the porting layer
 * is code someone will later believe. */
void set_fastforward_override(int v)
{
    (void)v;
}
