/*
 * OunceMTP - MTP object handle database.
 * SPDX-License-Identifier: ISC
 *
 * MTP addresses files by 32-bit handles that must stay stable for the whole
 * session. The SD card can hold hundreds of thousands of files, so nothing
 * is scanned up front: a directory is read only when the host lists it, and
 * each entry gets a handle the first time it is seen. Handles are never
 * reused within a session, so a host's stale cache can't hit the wrong file.
 *
 * Handle 0 is the internal root (the SD card), which MTP itself never sees
 * as an object: root-level objects report ParentObject = 0.
 */
#ifndef OUNCEMTP_OBJDB_H
#define OUNCEMTP_OBJDB_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define OBJ_ROOT    0u
#define OBJ_INVALID 0xFFFFFFFFu
#define OBJ_PATH_MAX 1024

typedef struct {
    char    *name;          /* NULL when the slot is dead               */
    uint32_t parent;
    uint32_t first_child;   /* singly linked list of children           */
    uint32_t next_sibling;
    uint32_t hash_next;     /* chain in the (parent,name) hash table     */
    uint32_t hash;
    uint8_t  is_dir;
    uint8_t  seen;          /* scratch flag for directory re-scans       */
} obj_t;

bool     objdb_init(const char *root_path);  /* e.g. "sdmc:" (no trailing /) */
void     objdb_reset(void);                  /* drop all handles (new session) */
void     objdb_free(void);

/* NULL if the handle is unknown or was removed. OBJ_ROOT is valid. */
obj_t   *objdb_get(uint32_t h);
bool     objdb_valid(uint32_t h);

/* Full filesystem path. Returns false if it does not fit. */
bool     objdb_path(uint32_t h, char *out, size_t out_size);
/* Path of a would-be child (for create/rename before the handle exists). */
bool     objdb_child_path(uint32_t parent, const char *name, char *out, size_t out_size);

uint32_t objdb_find(uint32_t parent, const char *name);
uint32_t objdb_add(uint32_t parent, const char *name, bool is_dir);   /* find-or-add */

/* Re-reads the directory from disk, adding new and dropping vanished
 * children. Fills *out (malloc'd) with child handles. Returns count or -1. */
int      objdb_list(uint32_t parent, uint32_t **out);

void     objdb_remove(uint32_t h);                       /* whole subtree */
bool     objdb_rename(uint32_t h, const char *new_name);
bool     objdb_move(uint32_t h, uint32_t new_parent);
/* True if `h` is `ancestor` or lies below it. */
bool     objdb_is_within(uint32_t h, uint32_t ancestor);

uint32_t objdb_count(void);  /* live objects, for the UI / tests */

#endif
