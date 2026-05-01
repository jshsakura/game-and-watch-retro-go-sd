#ifndef AMSTRAD_CATALOG_H
#define AMSTRAD_CATALOG_H

#include "cap32.h"

#define CAT_MAX_ENTRY 64
#define CAT_NAME_SIZE 20

typedef struct
{
   char filename[CAT_NAME_SIZE];
   bool is_hidden;
   bool is_readonly;
} amsdos_entry_t;

typedef struct {
   bool has_cat_art;
   bool probe_cpm;
   // normal entries
   int last_entry;
   amsdos_entry_t dirent[CAT_MAX_ENTRY];
   int entries_listed_found;
   int entries_hidden_found;
   int first_listed_dirent;
   int first_hidden_dirent;
   int track_hidden_id;
   int track_listed_id;
} catalogue_info_t;

extern catalogue_info_t catalogue;

int catalog_probe(t_drive *drive, unsigned char user);

#endif /* AMSTRAD_CATALOG_H */