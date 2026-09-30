
cd_data_t *chd_parse(const char *fname);
cd_data_t *cue_parse(const char *fname);
void        cdparse_destroy(cd_data_t *data);

/* harness experiment: per-track postgap (chdman cues) without touching pico.h */
extern int cd_track_postgap[100];

