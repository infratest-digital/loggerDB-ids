#ifndef LOGGERDB_ENUM_H
#define LOGGERDB_ENUM_H

#include <stdint.h>
#include <time.h>
#include <sys/types.h>

#include "loggerDB/table.h"
#include "loggerDB/node.h"

// Enumeration and deletion over the time-addressed node tree.
//
// Point get/put by timestamp stays O(1) (the timestamp maps directly to a
// path). These operations are O(n) directory walks — loggerDB keeps no index,
// so "what exists" cannot be answered without scanning. End of iteration is
// signalled by returning LOGGERDB_NOTFOUND from *_next.

// ---- Iterators -------------------------------------------------------

// Iterates table (sensor) names directly under the db directory.
// Fields are private to the implementation.
typedef struct loggerdb_table_iter {
    void*   _dir;
    char*   _db_path;
    uint8_t init;
} loggerdb_table_iter;

// Iterates node timestamps within one table, ascending, optionally bounded to
// [start, end). start == 0 means "from the earliest", end == 0 means "through
// the newest". The matching node times are collected at open() time. Fields
// are private to the implementation.
typedef struct loggerdb_node_iter {
    time_t* _times;   // ascending, malloc'd at open
    int     _count;
    int     _idx;
    uint8_t init;
} loggerdb_node_iter;

int ldb_table_iter_open (loggerdb* db, loggerdb_table_iter* it);
// name_out gets the next table name (NUL-terminated, truncated to name_size).
// Returns LOGGERDB_OK, LOGGERDB_NOTFOUND at end, or an error status.
int ldb_table_iter_next (loggerdb_table_iter* it, char* name_out, size_t name_size);
int ldb_table_iter_close(loggerdb_table_iter* it);

int ldb_node_iter_open (loggerdb_table* table, time_t start, time_t end, loggerdb_node_iter* it);
// time_out gets the next node's minute-aligned timestamp (ascending).
// Returns LOGGERDB_OK, LOGGERDB_NOTFOUND at end, or an error status.
int ldb_node_iter_next (loggerdb_node_iter* it, time_t* time_out);
int ldb_node_iter_close(loggerdb_node_iter* it);

// ---- Deletion --------------------------------------------------------

// Remove one node: its field files and the minute directory. No open handle
// needed. Now-empty parent dirs (HH/DD/MM/YYYY) are pruned. NOTFOUND if the
// node does not exist.
int ldb_node_remove       (loggerdb_table* table, time_t time);

// Remove every node in [start, end) for the table.
int ldb_table_remove_range(loggerdb_table* table, time_t start, time_t end);

// Remove an entire table (all nodes) and its directory.
int ldb_table_delete      (loggerdb* db, const char* name);

// Remove a single field file within an open node, leaving other fields.
// Only needed if multiple logical series share one node.
int ldb_node_field_remove (loggerdb_node* node, const char* field);

// ---- Convenience (built on the node iterator) ------------------------

// LOGGERDB_OK if the table has at least one node, LOGGERDB_NOTFOUND if empty.
int     ldb_table_has_data   (loggerdb_table* table);

// Count of nodes in [start, end). On error returns a negative -status.
ssize_t ldb_table_node_count (loggerdb_table* table, time_t start, time_t end);

// Earliest and latest node timestamps. LOGGERDB_NOTFOUND if the table is empty.
int     ldb_table_time_bounds(loggerdb_table* table, time_t* first_out, time_t* last_out);

// ---- Table-level metadata --------------------------------------------

// A single "metadata" file in the table directory. Unlike node metadata this
// is not size-capped; it holds the per-sensor session index.
ssize_t ldb_table_metadata_read (loggerdb_table* table, void* ptr, size_t size);
ssize_t ldb_table_metadata_write(loggerdb_table* table, void* ptr, size_t size);

#endif
