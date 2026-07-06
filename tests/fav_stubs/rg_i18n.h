#ifndef STUB_FAV_RG_I18N_H
#define STUB_FAV_RG_I18N_H
/* rg_favorites.c only reads curr_lang->s_favorite / s_no_favorite (tab
 * status text in favorites_refresh_tab). */
typedef struct {
    const char *s_favorite;
    const char *s_no_favorite;
} lang_t;

extern const lang_t *curr_lang;

#endif
