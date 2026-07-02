#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef _MSC_VER
#include <windows.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

#include "loggerDB/db.h"
#include "loggerDB/status.h"

#include "path.h"

#define LDB_PATH_SEP "/"

// Returns non-zero if path exists
int ldb_path_exists(const char* path)
{
#ifdef _MSC_VER
    DWORD a = GetFileAttributesA(path);
    if (a == INVALID_FILE_ATTRIBUTES)
        return 0;
    return (a & (FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_DIRECTORY)) != 0;
#else
    struct stat sb = {0};
    return stat(path, &sb) == 0;
#endif
}

// Returns non-zero if path is a file
int ldb_path_is_file(const char* path)
{
#ifdef _MSC_VER
    DWORD a = GetFileAttributesA(path);
    if (a == INVALID_FILE_ATTRIBUTES)
        return 0;
    return (a & FILE_ATTRIBUTE_NORMAL) != 0;
#else
    struct stat sb = {0};
    stat(path, &sb);
    return S_ISREG(sb.st_mode);
#endif
}

// Returns non-zero if path is a directory
int ldb_path_is_dir(const char* path)
{
#ifdef _MSC_VER
    DWORD a = GetFileAttributesA(path);
    if (a == INVALID_FILE_ATTRIBUTES)
        return 0;
    return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat sb = {0};
    stat(path, &sb);
    return S_ISDIR(sb.st_mode);
#endif
}

char* ldb_path_join(const char* s1, const char* s2)
{
    if (!s1 || !s2)
        return NULL;

    size_t len = strlen(s1) + strlen(LDB_PATH_SEP) + strlen(s2);
    if (!len)
        return NULL;

    char* o = malloc(len+1);
    *o = '\0';

    strcat(o, s1);
    strcat(o, LDB_PATH_SEP);
    strcat(o, s2);

    return o;
}

// Directory enumeration and deletion. loggerDB is POSIX-only in practice
// (node.c uses the two-arg mkdir()), so these use dirent/unistd directly.

int ldb_path_rmtree(const char* path)
{
    if (!path)
        return LOGGERDB_INVALID;

    if (!ldb_path_exists(path))
        return LOGGERDB_NOTFOUND;

    if (ldb_path_is_dir(path))
    {
        DIR* d = opendir(path);
        if (d)
        {
            struct dirent* e;
            while ((e = readdir(d)))
            {
                if (e->d_name[0] == '.' &&
                    (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0')))
                    continue;

                char* child = ldb_path_join(path, e->d_name);
                if (child)
                {
                    ldb_path_rmtree(child);
                    free(child);
                }
            }
            closedir(d);
        }

        if (rmdir(path) != 0)
            return LOGGERDB_IOERROR;
    }
    else
    {
        if (remove(path) != 0)
            return LOGGERDB_IOERROR;
    }

    return LOGGERDB_OK;
}

static int ldb_name_cmp(const void* a, const void* b)
{
    return strcmp((const char*)a, (const char*)b);
}

int ldb_path_list_dirs(const char* path, char (*names)[LDB_DIR_NAME_MAX], int max, int* count_out)
{
    if (!path || !names || !count_out)
        return LOGGERDB_INVALID;

    *count_out = 0;

    DIR* d = opendir(path);
    if (!d)
        return LOGGERDB_OK; // missing directory == empty

    int n = 0;
    struct dirent* e;
    while ((e = readdir(d)))
    {
        if (e->d_name[0] == '.' &&
            (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0')))
            continue;

        char* full = ldb_path_join(path, e->d_name);
        int is_dir = full && ldb_path_is_dir(full);
        free(full);
        if (!is_dir)
            continue;

        if (n < max)
        {
            strncpy(names[n], e->d_name, LDB_DIR_NAME_MAX - 1);
            names[n][LDB_DIR_NAME_MAX - 1] = '\0';
            ++n;
        }
    }
    closedir(d);

    // Node component names are zero-padded, so lexical order == chronological.
    qsort(names, n, LDB_DIR_NAME_MAX, ldb_name_cmp);

    *count_out = n;
    return LOGGERDB_OK;
}
