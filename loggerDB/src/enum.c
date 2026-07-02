#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

#include "loggerDB/table.h"
#include "loggerDB/node.h"
#include "loggerDB/db.h"
#include "loggerDB/status.h"
#include "loggerDB/enum.h"

#include "path.h"

// Windows/mingw opens text-mode by default, which mangles binary payloads.
// O_BINARY does not exist on POSIX / the device, where it is a harmless no-op.
#ifndef O_BINARY
#define O_BINARY 0
#endif

// Max sub-directories read at one tree level. Node levels never exceed 60
// (minutes); years/sensor-tables beyond this are truncated (see docs).
#define LDB_MAX_LEVEL_ENTRIES 64

// ---------------------------------------------------------------------
// Streaming node walk (ascending). fn returns non-zero to stop early.
// ---------------------------------------------------------------------

typedef int (*ldb_node_visit)(time_t t, void* ctx);

static int walk_rec(const char* path, int level, int comps[5],
                    time_t start, time_t end,
                    ldb_node_visit fn, void* ctx, int* stop)
{
    char (*names)[LDB_DIR_NAME_MAX] =
        malloc(sizeof(char[LDB_MAX_LEVEL_ENTRIES][LDB_DIR_NAME_MAX]));
    if (!names)
        return LOGGERDB_NOMEM;

    int count = 0;
    int rc = ldb_path_list_dirs(path, names, LDB_MAX_LEVEL_ENTRIES, &count);
    if (rc != LOGGERDB_OK)
    {
        free(names);
        return rc;
    }

    for (int i = 0; i < count && !*stop; ++i)
    {
        comps[level] = atoi(names[i]);

        char* child = ldb_path_join(path, names[i]);
        if (!child)
        {
            free(names);
            return LOGGERDB_NOMEM;
        }

        if (level == 4)
        {
            time_t t = ldb_time_from_civil(comps[0], comps[1], comps[2],
                                           comps[3], comps[4], 0);
            if ((start == 0 || t >= start) && (end == 0 || t < end))
            {
                if (fn(t, ctx))
                    *stop = 1;
            }
        }
        else
        {
            rc = walk_rec(child, level + 1, comps, start, end, fn, ctx, stop);
        }

        free(child);

        if (rc != LOGGERDB_OK && rc != LOGGERDB_NOTFOUND)
        {
            free(names);
            return rc;
        }
        rc = LOGGERDB_OK;
    }

    free(names);
    return LOGGERDB_OK;
}

static int walk_nodes(loggerdb_table* table, time_t start, time_t end,
                      ldb_node_visit fn, void* ctx)
{
    if (!table || !table->init)
        return LOGGERDB_INVALID;

    int comps[5] = {0};
    int stop = 0;
    return walk_rec(table->path, 0, comps, start, end, fn, ctx, &stop);
}

// ---------------------------------------------------------------------
// Table (sensor) iterator — lazy dirent scan of the db directory.
// ---------------------------------------------------------------------

int ldb_table_iter_open(loggerdb* db, loggerdb_table_iter* it)
{
    if (!db || !it)
        return LOGGERDB_INVALID;

    it->_db_path = db->path;
    it->_dir = opendir(db->path); // NULL if empty/missing -> next() ends
    it->init = 1;
    return LOGGERDB_OK;
}

int ldb_table_iter_next(loggerdb_table_iter* it, char* name_out, size_t name_size)
{
    if (!it || !it->init || !name_out || name_size == 0)
        return LOGGERDB_INVALID;

    DIR* d = (DIR*)it->_dir;
    if (!d)
        return LOGGERDB_NOTFOUND;

    struct dirent* e;
    while ((e = readdir(d)))
    {
        if (e->d_name[0] == '.' &&
            (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0')))
            continue;

        char* full = ldb_path_join(it->_db_path, e->d_name);
        int is_dir = full && ldb_path_is_dir(full);
        free(full);
        if (!is_dir)
            continue;

        strncpy(name_out, e->d_name, name_size - 1);
        name_out[name_size - 1] = '\0';
        return LOGGERDB_OK;
    }

    return LOGGERDB_NOTFOUND;
}

int ldb_table_iter_close(loggerdb_table_iter* it)
{
    if (!it)
        return LOGGERDB_INVALID;

    if (it->_dir)
        closedir((DIR*)it->_dir);
    it->_dir = NULL;
    it->_db_path = NULL;
    it->init = 0;
    return LOGGERDB_OK;
}

// ---------------------------------------------------------------------
// Node iterator — collects matching times (ascending) at open().
// ---------------------------------------------------------------------

struct collect { time_t* arr; int count; int cap; };

static int visit_collect(time_t t, void* ctx)
{
    struct collect* c = ctx;
    if (c->count == c->cap)
    {
        int nc = c->cap ? c->cap * 2 : 64;
        time_t* na = realloc(c->arr, (size_t)nc * sizeof(time_t));
        if (!na)
            return 1; // stop; treated as allocation failure by caller
        c->arr = na;
        c->cap = nc;
    }
    c->arr[c->count++] = t;
    return 0;
}

int ldb_node_iter_open(loggerdb_table* table, time_t start, time_t end,
                       loggerdb_node_iter* it)
{
    if (!table || !it)
        return LOGGERDB_INVALID;

    struct collect c = {NULL, 0, 0};
    int rc = walk_nodes(table, start, end, visit_collect, &c);
    if (rc != LOGGERDB_OK)
    {
        free(c.arr);
        return rc;
    }

    it->_times = c.arr;
    it->_count = c.count;
    it->_idx = 0;
    it->init = 1;
    return LOGGERDB_OK;
}

int ldb_node_iter_next(loggerdb_node_iter* it, time_t* time_out)
{
    if (!it || !it->init)
        return LOGGERDB_INVALID;

    if (it->_idx >= it->_count)
        return LOGGERDB_NOTFOUND;

    if (time_out)
        *time_out = it->_times[it->_idx];
    it->_idx++;
    return LOGGERDB_OK;
}

int ldb_node_iter_close(loggerdb_node_iter* it)
{
    if (!it)
        return LOGGERDB_INVALID;

    free(it->_times);
    it->_times = NULL;
    it->_count = 0;
    it->_idx = 0;
    it->init = 0;
    return LOGGERDB_OK;
}

// ---------------------------------------------------------------------
// Deletion.
// ---------------------------------------------------------------------

// After removing a node dir, rmdir now-empty parents (HH, DD, MM, YYYY).
static void prune_empty_parents(const char* table_path, const char* node_sub)
{
    char sub[18];
    strncpy(sub, node_sub, sizeof(sub) - 1);
    sub[sizeof(sub) - 1] = '\0';

    for (int i = 0; i < 4; ++i)
    {
        char* slash = strrchr(sub, '/');
        if (!slash)
            break;
        *slash = '\0';

        char* p = ldb_path_join(table_path, sub);
        if (!p)
            break;

        int r = rmdir(p); // fails (non-empty) -> stop pruning
        free(p);
        if (r != 0)
            break;
    }
}

int ldb_node_remove(loggerdb_table* table, time_t time)
{
    if (!table || !table->init)
        return LOGGERDB_INVALID;

    char sub[18];
    ldb_time_to_path(time, sub);

    char* node_path = ldb_path_join(table->path, sub);
    if (!node_path)
        return LOGGERDB_NOMEM;

    if (!ldb_path_is_dir(node_path))
    {
        free(node_path);
        return LOGGERDB_NOTFOUND;
    }

    int rc = ldb_path_rmtree(node_path);
    free(node_path);
    if (rc != LOGGERDB_OK)
        return rc;

    prune_empty_parents(table->path, sub);
    return LOGGERDB_OK;
}

int ldb_table_remove_range(loggerdb_table* table, time_t start, time_t end)
{
    if (!table || !table->init)
        return LOGGERDB_INVALID;

    struct collect c = {NULL, 0, 0};
    int rc = walk_nodes(table, start, end, visit_collect, &c);
    if (rc != LOGGERDB_OK)
    {
        free(c.arr);
        return rc;
    }

    for (int i = 0; i < c.count; ++i)
    {
        int r = ldb_node_remove(table, c.arr[i]);
        if (r != LOGGERDB_OK && r != LOGGERDB_NOTFOUND)
        {
            free(c.arr);
            return r;
        }
    }

    free(c.arr);
    return LOGGERDB_OK;
}

int ldb_table_delete(loggerdb* db, const char* name)
{
    if (!db || !name)
        return LOGGERDB_INVALID;

    char* table_path = ldb_path_join(db->path, name);
    if (!table_path)
        return LOGGERDB_NOMEM;

    if (!ldb_path_is_dir(table_path))
    {
        free(table_path);
        return LOGGERDB_NOTFOUND;
    }

    int rc = ldb_path_rmtree(table_path);
    free(table_path);
    return rc;
}

int ldb_node_field_remove(loggerdb_node* node, const char* field)
{
    if (!node || !node->init || !field)
        return LOGGERDB_INVALID;

    char* field_path = ldb_path_join(node->path, field);
    if (!field_path)
        return LOGGERDB_NOMEM;

    int rc = LOGGERDB_OK;
    if (!ldb_path_exists(field_path))
        rc = LOGGERDB_NOTFOUND;
    else if (remove(field_path) != 0)
        rc = LOGGERDB_IOERROR;

    free(field_path);
    return rc;
}

// ---------------------------------------------------------------------
// Convenience (built on the streaming walk).
// ---------------------------------------------------------------------

static int visit_first(time_t t, void* ctx)
{
    (void)t;
    *(int*)ctx = 1;
    return 1; // stop after first
}

int ldb_table_has_data(loggerdb_table* table)
{
    int found = 0;
    int rc = walk_nodes(table, 0, 0, visit_first, &found);
    if (rc != LOGGERDB_OK)
        return rc;
    return found ? LOGGERDB_OK : LOGGERDB_NOTFOUND;
}

static int visit_count(time_t t, void* ctx)
{
    (void)t;
    (*(ssize_t*)ctx)++;
    return 0;
}

ssize_t ldb_table_node_count(loggerdb_table* table, time_t start, time_t end)
{
    ssize_t n = 0;
    int rc = walk_nodes(table, start, end, visit_count, &n);
    if (rc != LOGGERDB_OK)
        return -rc;
    return n;
}

struct bounds { time_t first; time_t last; int any; };

static int visit_bounds(time_t t, void* ctx)
{
    struct bounds* b = ctx;
    if (!b->any)
    {
        b->first = t;
        b->any = 1;
    }
    b->last = t; // ascending, so this ends on the newest
    return 0;
}

int ldb_table_time_bounds(loggerdb_table* table, time_t* first_out, time_t* last_out)
{
    struct bounds b = {0, 0, 0};
    int rc = walk_nodes(table, 0, 0, visit_bounds, &b);
    if (rc != LOGGERDB_OK)
        return rc;
    if (!b.any)
        return LOGGERDB_NOTFOUND;
    if (first_out)
        *first_out = b.first;
    if (last_out)
        *last_out = b.last;
    return LOGGERDB_OK;
}

// ---------------------------------------------------------------------
// Table-level metadata — a single "metadata" file in the table directory,
// not size-capped (holds the per-sensor session index).
// ---------------------------------------------------------------------

ssize_t ldb_table_metadata_write(loggerdb_table* table, void* ptr, size_t size)
{
    if (!table || !table->init)
        return -LOGGERDB_INVALID;

    char* path = ldb_path_join(table->path, "metadata");
    if (!path)
        return -LOGGERDB_NOMEM;

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    free(path);
    if (fd < 0)
        return -LOGGERDB_FDERR;

    ssize_t r = write(fd, ptr, size);
    if (close(fd) < 0)
        return -LOGGERDB_FDBAD;
    return r;
}

ssize_t ldb_table_metadata_read(loggerdb_table* table, void* ptr, size_t size)
{
    if (!table || !table->init)
        return -LOGGERDB_INVALID;

    char* path = ldb_path_join(table->path, "metadata");
    if (!path)
        return -LOGGERDB_NOMEM;

    int fd = open(path, O_RDONLY | O_BINARY);
    free(path);
    if (fd < 0)
        return -LOGGERDB_FDERR; // absent == not written yet

    ssize_t r = read(fd, ptr, size);
    if (close(fd) < 0)
        return -LOGGERDB_FDBAD;
    return r;
}
