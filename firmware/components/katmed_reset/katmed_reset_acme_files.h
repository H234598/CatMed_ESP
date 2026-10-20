#ifndef KATMED_RESET_ACME_FILES_H
#define KATMED_RESET_ACME_FILES_H

#include <stdbool.h>

/* Only the journal's file stage calls the production entry point. */
bool katmed_reset_acme_files(void);

/* The same bounded cleanup accepts a synthetic directory in host tests. */
bool katmed_reset_acme_files_at(const char *directory_path);

#endif
