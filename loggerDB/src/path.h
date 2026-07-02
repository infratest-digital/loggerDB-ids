#ifndef LOGGERDB_PATH_INTERNAL_H
#define LOGGERDB_PATH_INTERNAL_H

#include <time.h>

// Max length of a single path component read from disk (node dirs are 4-digit
// years / 2-digit fields; table names are short too).
#define LDB_DIR_NAME_MAX 32

int ldb_path_exists(const char* path);
int ldb_path_is_file(const char* path);
int ldb_path_is_dir(const char* path);
char* ldb_path_join(const char* s1, const char* s2);

// Recursively delete a file or directory tree. Returns a LOGGERDB_* status.
int ldb_path_rmtree(const char* path);

// Read the immediate sub-directory names of `path` into names[] (each slot
// LDB_DIR_NAME_MAX), sorted ascending, up to `max`. Sets *count_out. A missing
// directory is treated as empty (LOGGERDB_OK, count 0).
int ldb_path_list_dirs(const char* path, char (*names)[LDB_DIR_NAME_MAX], int max, int* count_out);

// Format a node's sub-path "YYYY/MM/DD/HH/MM" (18-byte buffer incl. NUL).
// Defined in node.c; ldb_time_from_civil is the public inverse (node.h).
void ldb_time_to_path(time_t time, char* buff);

#endif
