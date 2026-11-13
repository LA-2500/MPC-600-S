// Folders and files for Sample Select: the places sounds can come from (SD cards and USB
// drives under /media, the internal storage at /data), and one folder's contents.
#pragma once

#define FILES_PATH 512 // a whole path
#define FILES_NAME 128 // one name
#define FILES_MAX 512  // entries in one folder (the rest are left out)
#define FILES_PLACES 8

struct file_entry {
    char name[FILES_NAME];
    unsigned char dir;   // a folder (or a place)
    unsigned char up;    // ".." : the folder above, or the list of places
    unsigned char place; // one of the places, in the list of them
};

struct files {
    char cwd[FILES_PATH]; // the folder shown; "" for the list of places
    struct file_entry entries[FILES_MAX];
    int count, cursor, top; // top: the first row shown
    char places[FILES_PLACES][FILES_PATH];
    int nplaces;
    const char *const *ext; // the files listed, by extension (any case); NULL lists every file
};

// The places: root alone if given (tests), else every folder in /media, then /data. Shows them.
void files_init(struct files *f, const char *root, const char *const *ext);
// Shows a folder ("" the places). 0, or -1 when it cannot be read (nothing changes then).
int files_open(struct files *f, const char *path);
// The place a path is the top of, or -1.
int files_place_of(const struct files *f, const char *path);
