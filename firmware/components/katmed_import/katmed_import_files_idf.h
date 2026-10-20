#pragma once
#include "katmed_import.h"

bool katmed_import_files_empty(void *, size_t content_bytes, size_t count);
bool katmed_import_file_stage(void *, unsigned ordinal, const KatmedImportFile *,
                              const KatmedImportInput *);
bool katmed_import_file_publish(void *, unsigned ordinal, const KatmedImportFile *);
bool katmed_import_file_final(void *, unsigned ordinal, const KatmedImportFile *);
bool katmed_import_file_abort(void *, unsigned ordinal, const KatmedImportFile *);
bool katmed_import_files_unmount(void);
