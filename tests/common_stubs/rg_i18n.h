/* Host-test stub for Core/Inc/retro-go/rg_i18n.h -- only the turbo-button
 * settings common.c's macro-handling code calls. */
#ifndef STUB_RG_I18N_H
#define STUB_RG_I18N_H
#include <stdint.h>
#include <stdbool.h>

bool odroid_button_turbos(void);
int8_t odroid_settings_turbo_buttons_get(void);
void odroid_settings_turbo_buttons_set(int8_t turbo_buttons);

#endif
