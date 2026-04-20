#include "index.h"
#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;

    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }

    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    printf("  (nothing to show)\n\n");

    printf("Untracked files:\n");

    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {

            if (strcmp(ent->d_name, ".") == 0 ||
                strcmp(ent->d_name, "..") == 0 ||
                strcmp(ent->d_name, ".pes") == 0 ||
                strcmp(ent->d_name, "pes") == 0 ||
                strstr(ent->d_name, ".o") != NULL)
                continue;

            int tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    tracked = 1;
                    break;
                }
            }

            if (!tracked) {
                printf("  untracked:  %s\n", ent->d_name);
            }
        }
        closedir(dir);
    }

    printf("\n");
    return 0;
}

// ─── INTERNAL ────────────────────────────────────────────────────────────────

static int compare_index_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path,
                  ((const IndexEntry *)b)->path);
}

// ─── LOAD ────────────────────────────────────────────────────────────────────

int index_load(Index *index) {
    index->count = 0;

    FILE *f = fopen(".pes/index", "r");
    if (!f) return 0;

    char hex[65];

    while (fscanf(f, "%o %64s %lu %u %[^\n]\n",
                  &index->entries[index->count].mode,
                  hex,
                  &index->entries[index->count].mtime_sec,
                  &index->entries[index->count].size,
                  index->entries[index->count].path) == 5) {

        hex_to_hash(hex, &index->entries[index->count].hash);
        index->count++;
    }

    fclose(f);
    return 0;
}

// ─── SAVE ────────────────────────────────────────────────────────────────────

int index_save(const Index *index) {
    Index *sorted = malloc(sizeof(Index));
    if (!sorted) return -1;

    *sorted = *index;

    qsort(sorted->entries, sorted->count,
          sizeof(IndexEntry), compare_index_entries);

    FILE *f = fopen(".pes/index.tmp", "w");
    if (!f) {
        free(sorted);
        return -1;
    }

    char hex[65];

    for (int i = 0; i < sorted->count; i++) {
        hash_to_hex(&sorted->entries[i].hash, hex);

        fprintf(f, "%o %s %lu %u %s\n",
                sorted->entries[i].mode,
                hex,
                sorted->entries[i].mtime_sec,
                sorted->entries[i].size,
                sorted->entries[i].path);
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    free(sorted);

    rename(".pes/index.tmp", ".pes/index");
    return 0;
}

// ─── ADD (CORRECT FOR YOUR OBJECT.C) ─────────────────────────────────────────

int index_add(Index *index, const char *path) {
    struct stat st;

    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        fprintf(stderr, "error: invalid file '%s'\n", path);
        return -1;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return -1;
    }

    unsigned char *data = malloc(st.st_size);
    if (!data) {
        fclose(f);
        return -1;
    }

    if (st.st_size > 0 &&
        fread(data, 1, st.st_size, f) != (size_t)st.st_size) {
        free(data);
        fclose(f);
        return -1;
    }

    fclose(f);

    ObjectID hash;

    // ✅ CORRECT CALL (THIS WAS YOUR BUG)
    if (object_write(OBJ_BLOB, data, st.st_size, &hash) != 0) {
        free(data);
        fprintf(stderr, "error: object_write failed\n");
        return -1;
    }

    free(data);

    IndexEntry *entry = index_find(index, path);

    if (!entry) {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        entry = &index->entries[index->count++];
    }

    entry->mode = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;
    entry->hash = hash;
    entry->mtime_sec = st.st_mtime;
    entry->size = st.st_size;

    strncpy(entry->path, path, sizeof(entry->path) - 1);
    entry->path[sizeof(entry->path) - 1] = '\0';

    return index_save(index);
}