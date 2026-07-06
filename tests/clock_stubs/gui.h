#ifndef STUB_GUI_H
#define STUB_GUI_H
typedef struct { int _dummy; } tab_t;   /* opaque for the host harness */
tab_t *gui_get_current_tab(void);
void   gui_refresh_tab(tab_t *tab);
#endif
