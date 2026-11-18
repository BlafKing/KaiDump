/*
 * OunceMTP - MTP object handle database.
 * SPDX-License-Identifier: ISC
 *
 * Storage: one flat array indexed by handle (append-only, so a handle is just
 * an index and lookup is O(1)), a per-directory singly linked child list for
 * re-scans, and an open hash table keyed on (parent, name) so a re-listed
 * directory maps every name back to the handle the host already knows.
 * Memory per object is ~32 bytes plus the name.
 */
#include "objdb.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

static obj_t    *g_objs;
static uint32_t  g_count;     /* slots used (next handle)  */
static uint32_t  g_cap;
static uint32_t  g_live;
static uint32_t *g_buckets;
static uint32_t  g_nbuckets;  /* power of two              */
static char      g_root[OBJ_PATH_MAX];

static uint32_t name_hash(uint32_t parent, const char *name)
{
    uint32_t h = 2166136261u ^ (parent * 0x9E3779B1u);
    for (const unsigned char *s = (const unsigned char *)name; *s; s++) {
        h ^= *s;
        h *= 16777619u;
    }
    return h;
}

static bool rehash(uint32_t nb)
{
    uint32_t *b = malloc(nb * sizeof(uint32_t));
    if (!b) return false;
    memset(b, 0xFF, nb * sizeof(uint32_t));
    for (uint32_t i = 1; i < g_count; i++) {
        obj_t *o = &g_objs[i];
        if (!o->name) continue;
        uint32_t k = o->hash & (nb - 1);
        o->hash_next = b[k];
        b[k] = i;
    }
    free(g_buckets);
    g_buckets = b;
    g_nbuckets = nb;
    return true;
}

static void hash_unlink(uint32_t h)
{
    obj_t *o = &g_objs[h];
    uint32_t *pp = &g_buckets[o->hash & (g_nbuckets - 1)];
    while (*pp != OBJ_INVALID) {
        if (*pp == h) { *pp = o->hash_next; return; }
        pp = &g_objs[*pp].hash_next;
    }
}

static void child_unlink(uint32_t h)
{
    obj_t *o = &g_objs[h];
    uint32_t *pp = &g_objs[o->parent].first_child;
    while (*pp != OBJ_INVALID) {
        if (*pp == h) { *pp = o->next_sibling; return; }
        pp = &g_objs[*pp].next_sibling;
    }
}

static void child_link(uint32_t h, uint32_t parent)
{
    g_objs[h].parent = parent;
    g_objs[h].next_sibling = g_objs[parent].first_child;
    g_objs[parent].first_child = h;
}

static void hash_link(uint32_t h)
{
    obj_t *o = &g_objs[h];
    o->hash = name_hash(o->parent, o->name);
    uint32_t k = o->hash & (g_nbuckets - 1);
    o->hash_next = g_buckets[k];
    g_buckets[k] = h;
}

bool objdb_init(const char *root_path)
{
    snprintf(g_root, sizeof(g_root), "%s", root_path);
    /* strip trailing slashes: paths are built as root + "/" + name */
    size_t n = strlen(g_root);
    while (n > 0 && g_root[n - 1] == '/') g_root[--n] = 0;
    objdb_reset();
    return g_objs != NULL;
}

void objdb_free(void)
{
    if (g_objs) {
        for (uint32_t i = 1; i < g_count; i++) free(g_objs[i].name);
    }
    free(g_objs);
    free(g_buckets);
    g_objs = NULL; g_buckets = NULL;
    g_count = g_cap = g_live = g_nbuckets = 0;
}

void objdb_reset(void)
{
    objdb_free();
    g_cap = 1024;
    g_objs = calloc(g_cap, sizeof(obj_t));
    if (!g_objs) return;
    /* slot 0: root. Its name is "" so it never matches a lookup. */
    g_objs[0].name = NULL;
    g_objs[0].parent = OBJ_ROOT;
    g_objs[0].first_child = OBJ_INVALID;
    g_objs[0].next_sibling = OBJ_INVALID;
    g_objs[0].is_dir = 1;
    g_count = 1;
    rehash(1024);
}

obj_t *objdb_get(uint32_t h)
{
    if (h == OBJ_ROOT) return g_objs;
    if (h >= g_count || !g_objs[h].name) return NULL;
    return &g_objs[h];
}

bool objdb_valid(uint32_t h) { return objdb_get(h) != NULL; }

uint32_t objdb_count(void) { return g_live; }

bool objdb_path(uint32_t h, char *out, size_t out_size)
{
    /* Walk up collecting components, then emit root-first. Depth is bounded
     * by the path length limit, so a small fixed stack suffices. */
    uint32_t chain[128];
    int depth = 0;
    if (!objdb_get(h)) return false;
    while (h != OBJ_ROOT) {
        if (depth == 128) return false;
        chain[depth++] = h;
        h = g_objs[h].parent;
    }
    size_t len = strlen(g_root);
    if (len + 2 > out_size) return false;
    memcpy(out, g_root, len);
    if (depth == 0) { out[len++] = '/'; out[len] = 0; return true; }
    for (int i = depth - 1; i >= 0; i--) {
        const char *nm = g_objs[chain[i]].name;
        size_t nl = strlen(nm);
        if (len + 1 + nl + 1 > out_size) return false;
        out[len++] = '/';
        memcpy(out + len, nm, nl);
        len += nl;
    }
    out[len] = 0;
    return true;
}

bool objdb_child_path(uint32_t parent, const char *name, char *out, size_t out_size)
{
    if (!objdb_path(parent, out, out_size)) return false;
    size_t len = strlen(out);
    if (len > 0 && out[len - 1] == '/') len--;   /* root path ends in '/' */
    int w = snprintf(out + len, out_size - len, "/%s", name);
    return w > 0 && (size_t)w < out_size - len;
}

uint32_t objdb_find(uint32_t parent, const char *name)
{
    uint32_t hh = name_hash(parent, name);
    for (uint32_t i = g_buckets[hh & (g_nbuckets - 1)]; i != OBJ_INVALID; i = g_objs[i].hash_next) {
        obj_t *o = &g_objs[i];
        if (o->hash == hh && o->parent == parent && strcmp(o->name, name) == 0) return i;
    }
    return OBJ_INVALID;
}

uint32_t objdb_add(uint32_t parent, const char *name, bool is_dir)
{
    uint32_t h = objdb_find(parent, name);
    if (h != OBJ_INVALID) {
        if (g_objs[h].is_dir == (uint8_t)is_dir) return h;
        objdb_remove(h);   /* type changed on disk: new identity */
    }
    if (!objdb_get(parent) || !g_objs[parent].is_dir) return OBJ_INVALID;
    if (g_count == 0xFFFFFFF0u) return OBJ_INVALID;

    if (g_count == g_cap) {
        uint32_t nc = g_cap * 2;
        obj_t *n = realloc(g_objs, (size_t)nc * sizeof(obj_t));
        if (!n) return OBJ_INVALID;
        g_objs = n;
        g_cap = nc;
    }
    char *dup = strdup(name);
    if (!dup) return OBJ_INVALID;

    h = g_count++;
    obj_t *o = &g_objs[h];
    memset(o, 0, sizeof(*o));
    o->name = dup;
    o->is_dir = is_dir;
    o->first_child = OBJ_INVALID;
    child_link(h, parent);
    hash_link(h);
    g_live++;

    if (g_live > g_nbuckets - (g_nbuckets >> 2)) rehash(g_nbuckets * 2);
    return h;
}

void objdb_remove(uint32_t h)
{
    if (h == OBJ_ROOT || !objdb_get(h)) return;
    child_unlink(h);
    /* Iterative post-order-free walk of the subtree. The child links of a
     * node are consumed as we go, so no extra stack is needed. */
    uint32_t cur = h;
    for (;;) {
        obj_t *o = &g_objs[cur];
        if (o->first_child != OBJ_INVALID) {
            uint32_t c = o->first_child;
            o->first_child = g_objs[c].next_sibling;
            g_objs[c].parent = cur;   /* already true; keeps walk-up valid */
            cur = c;
            continue;
        }
        uint32_t up = o->parent;
        hash_unlink(cur);
        free(o->name);
        o->name = NULL;
        g_live--;
        if (cur == h) break;
        cur = up;
    }
}

bool objdb_rename(uint32_t h, const char *new_name)
{
    if (h == OBJ_ROOT || !objdb_get(h)) return false;
    uint32_t clash = objdb_find(g_objs[h].parent, new_name);
    if (clash != OBJ_INVALID && clash != h) objdb_remove(clash);
    char *dup = strdup(new_name);
    if (!dup) return false;
    hash_unlink(h);
    free(g_objs[h].name);
    g_objs[h].name = dup;
    hash_link(h);
    return true;
}

bool objdb_is_within(uint32_t h, uint32_t ancestor)
{
    for (int guard = 0; guard < 256; guard++) {
        if (h == ancestor) return true;
        if (h == OBJ_ROOT) return false;
        h = g_objs[h].parent;
    }
    return false;
}

bool objdb_move(uint32_t h, uint32_t new_parent)
{
    if (h == OBJ_ROOT || !objdb_get(h) || !objdb_get(new_parent)) return false;
    if (objdb_is_within(new_parent, h)) return false;
    uint32_t clash = objdb_find(new_parent, g_objs[h].name);
    if (clash != OBJ_INVALID && clash != h) objdb_remove(clash);
    hash_unlink(h);
    child_unlink(h);
    child_link(h, new_parent);
    hash_link(h);
    return true;
}

int objdb_list(uint32_t parent, uint32_t **out)
{
    char path[OBJ_PATH_MAX];
    *out = NULL;
    obj_t *p = objdb_get(parent);
    if (!p || !p->is_dir) return -1;
    if (!objdb_path(parent, path, sizeof(path))) return -1;

    DIR *d = opendir(path);
    if (!d) return -1;

    for (uint32_t c = g_objs[parent].first_child; c != OBJ_INVALID; c = g_objs[c].next_sibling)
        g_objs[c].seen = 0;

    uint32_t *arr = NULL, n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        bool is_dir;
#ifdef DT_DIR
        if (e->d_type == DT_DIR)      is_dir = true;
        else if (e->d_type == DT_REG) is_dir = false;
        else
#endif
        {
            char cp[OBJ_PATH_MAX];
            struct stat st;
            if (!objdb_child_path(parent, e->d_name, cp, sizeof(cp)) || stat(cp, &st) != 0) continue;
            is_dir = S_ISDIR(st.st_mode);
        }
        uint32_t h = objdb_add(parent, e->d_name, is_dir);   /* may realloc g_objs */
        if (h == OBJ_INVALID) continue;
        g_objs[h].seen = 1;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            uint32_t *na = realloc(arr, cap * sizeof(uint32_t));
            if (!na) break;
            arr = na;
        }
        arr[n++] = h;
    }
    closedir(d);

    /* Drop children that vanished from disk (deleted on the console side
     * or by another tool) so their handles stop resolving. */
    for (;;) {
        uint32_t gone = OBJ_INVALID;
        for (uint32_t c = g_objs[parent].first_child; c != OBJ_INVALID; c = g_objs[c].next_sibling)
            if (!g_objs[c].seen) { gone = c; break; }
        if (gone == OBJ_INVALID) break;
        objdb_remove(gone);
    }

    *out = arr;
    return (int)n;
}
