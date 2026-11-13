// Folders and files for Sample Select: see files.h.
#include "files.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static int is_folder(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void add_place(struct files *f, const char *path) {
    if (f->nplaces < FILES_PLACES && is_folder(path)) snprintf(f->places[f->nplaces++], FILES_PATH, "%s", path);
}

void files_init(struct files *f, const char *root, const char *const *ext) {
    memset(f, 0, sizeof *f);
    f->ext = ext;
    if (root) {
        add_place(f, root);
    } else {
        DIR *d = opendir("/media"); // each card or drive mounts as a folder here
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char p[FILES_PATH];
            snprintf(p, sizeof p, "/media/%.200s", e->d_name);
            add_place(f, p);
        }
        if (d) closedir(d);
        add_place(f, "/data"); // the internal storage
    }
    files_open(f, "");
}

int files_place_of(const struct files *f, const char *path) {
    for (int i = 0; i < f->nplaces; i++)
        if (strcmp(path, f->places[i]) == 0) return i;
    return -1;
}

static int wanted(const struct files *f, const char *name) {
    if (!f->ext) return 1;
    size_t n = strlen(name);
    for (int i = 0; f->ext[i]; i++) {
        size_t e = strlen(f->ext[i]);
        if (n > e && strcasecmp(name + n - e, f->ext[i]) == 0) return 1;
    }
    return 0;
}

// Folders before files, each A to Z without regard to case; ".." first of all.
static int order(const void *a, const void *b) {
    const struct file_entry *x = a, *y = b;
    if (x->up != y->up) return y->up - x->up;
    if (x->dir != y->dir) return y->dir - x->dir;
    return strcasecmp(x->name, y->name);
}

int files_open(struct files *f, const char *path) {
    if (!path[0]) { // the places
        f->cwd[0] = 0;
        f->count = 0;
        for (int i = 0; i < f->nplaces; i++) {
            struct file_entry *e = &f->entries[f->count++];
            const char *slash = strrchr(f->places[i], '/');
            snprintf(e->name, FILES_NAME, "%s", slash && slash[1] ? slash + 1 : f->places[i]);
            if (strcmp(f->places[i], "/data") == 0) snprintf(e->name, FILES_NAME, "Internal");
            e->dir = 1, e->up = 0, e->place = 1;
        }
        f->cursor = f->top = 0;
        return 0;
    }
    DIR *d = opendir(path);
    if (!d) return -1;
    snprintf(f->cwd, FILES_PATH, "%s", path);
    f->count = 0;
    struct file_entry *up = &f->entries[f->count++]; // back up: to the folder above, or the places
    snprintf(up->name, FILES_NAME, "..");
    up->dir = 1, up->up = 1, up->place = 0;
    struct dirent *e;
    while ((e = readdir(d)) && f->count < FILES_MAX) {
        if (e->d_name[0] == '.') continue; // hidden, and . and ..
        char full[FILES_PATH + FILES_NAME];
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        int folder = is_folder(full);
        if (!folder && !wanted(f, e->d_name)) continue;
        struct file_entry *x = &f->entries[f->count++];
        snprintf(x->name, FILES_NAME, "%s", e->d_name);
        x->dir = (unsigned char)folder, x->up = 0, x->place = 0;
    }
    closedir(d);
    qsort(f->entries, (size_t)f->count, sizeof f->entries[0], order);
    f->cursor = f->top = 0;
    return 0;
}
