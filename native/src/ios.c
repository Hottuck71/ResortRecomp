/* Answers the requests the game sends to IOS, following the public documentation at
   wiibrew.org/wiki/IOS. /dev/di reads the extracted disc, /dev/fs and NAND files use a plain
   folder, and /dev/es reports facts from the disc's own TMD. Everything else says yes. */
#include <dirent.h>
#include <direct.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "host.h"

#define MAX_HANDLES 64
#define MAX_LOG 6000
#define FS_ENOENT (-106)
#define FS_EEXIST (-105)
#define FS_EINVAL (-101)
#define FS_ENOENT_OPEN (-106)

typedef enum { DEV_OTHER, DEV_DI, DEV_ES, DEV_FS, DEV_EVENTHOOK, DEV_FILE } Kind;

typedef struct {
    int used;
    Kind kind;
    char path[64];
    FILE* file;
} Handle;

typedef struct {
    u8* data;
    u32 size;
} Vec;

static Handle handles[MAX_HANDLES];
static char nand_root[512];
static u32 log_count;

static int host_path(const char* guest, char* out, size_t size) {
    if (guest[0] != '/' || strstr(guest, "..") || strchr(guest, ':') || strchr(guest, '\\'))
        return 0;
    snprintf(out, size, "%s%s", nand_root, guest);
    return 1;
}

static void make_parents(const char* path) {
    char copy[768];
    snprintf(copy, sizeof copy, "%s", path);
    for (char* p = copy + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        _mkdir(copy);
        *p = '/';
    }
}

void ios_init(const char* root) {
    snprintf(nand_root, sizeof nand_root, "%s", root);
    char probe[768];
    snprintf(probe, sizeof probe, "%s/tmp/", nand_root);
    make_parents(probe);
    /* The game's own data folder, which a console makes when the game is installed. The game writes its save
       there but never makes the folder: without it, it shows "Could not access Wii system memory". */
    snprintf(probe, sizeof probe, "%s/title/00010000/525a5445/data/", nand_root);
    make_parents(probe);
}

static int is_dir(const char* path) {
    struct stat s;
    return !stat(path, &s) && (s.st_mode & S_IFDIR);
}

static int exists(const char* path) {
    struct stat s;
    return !stat(path, &s);
}

static void delete_tree(const char* path) {
    if (!is_dir(path)) {
        remove(path);
        return;
    }
    DIR* d = opendir(path);
    if (d) {
        struct dirent* e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char child[768];
            snprintf(child, sizeof child, "%s/%s", path, e->d_name);
            delete_tree(child);
        }
        closedir(d);
    }
    _rmdir(path);
}

static s32 open_path(const char* path, u32 mode) {
    int fd;
    for (fd = 0; fd < MAX_HANDLES; fd++)
        if (!handles[fd].used) break;
    if (fd == MAX_HANDLES) return -1;
    Handle* h = &handles[fd];
    memset(h, 0, sizeof *h);
    snprintf(h->path, sizeof h->path, "%s", path);
    if (!strncmp(path, "/dev/", 5)) {
        h->kind = !strcmp(path, "/dev/di") ? DEV_DI : !strcmp(path, "/dev/es") ? DEV_ES
                : !strcmp(path, "/dev/fs") ? DEV_FS
                : !strcmp(path, "/dev/stm/eventhook") ? DEV_EVENTHOOK : DEV_OTHER;
    } else {
        char host[768];
        if (!host_path(path, host, sizeof host)) return FS_EINVAL;
        if (is_dir(host)) return FS_ENOENT_OPEN;
        h->file = fopen(host, mode == 1 ? "rb" : "r+b");
        if (!h->file) return FS_ENOENT_OPEN;
        h->kind = DEV_FILE;
    }
    h->used = 1;
    return fd;
}

#define DRIVE_OPEN_LIMIT 0x50000ull
#define DRIVE_LBA_OUT_OF_RANGE 0x052100u
#define DRIVE_INVALID_COMMAND_FOR_DISC 0x053100u
static u32 drive_error;

static s32 di_ioctl(u32 number, const u8* in, u8* out, u32 out_size) {
    u32 length = in ? (u32)load_be(in + 4, 4) : 0;
    u64 offset = in ? load_be(in + 8, 4) << 2 : 0;
    if (out) memset(out, 0, out_size);
    switch (number) {
    case 0x12: /* inquiry */
        if (out && out_size >= 8) {
            store_be(out, 2, 2);
            store_be(out + 2, 0x20, 2);
            store_be(out + 4, 0x20060526u, 4);
        }
        return 1;
    case 0x70: /* read disc ID */
        if (out) memcpy(out, disc_header(), out_size < 0x20 ? out_size : 0x20);
        return 1;
    case 0x71: /* read from the open partition */
        if (!out) return 2;
        if (length > out_size) length = out_size;
        return disc_read(offset, out, length) ? 1 : 2;
    case 0x8D: /* read outside the partition: a genuine drive refuses this range */
        if (offset >= DRIVE_OPEN_LIMIT) {
            drive_error = DRIVE_LBA_OUT_OF_RANGE;
            return 2;
        }
        fprintf(report, "IOS: unencrypted read %llX size %u answered with zeros\n",
                (unsigned long long)offset, length);
        return 1;
    case 0xA4: /* report key: only DVD-Video discs answer this */
        drive_error = DRIVE_INVALID_COMMAND_FOR_DISC;
        return 2;
    case 0xE0: /* request error */
        if (out && out_size >= 4) store_be(out, drive_error, 4);
        drive_error = 0;
        return 1;
    case 0x88: /* cover status: a disc is in */
        if (out && out_size >= 4) store_be(out, 2, 4);
        return 1;
    default:
        return 1;
    }
}

static s32 di_ioctlv(u32 number, Vec* v, u32 in_count, u32 io_count) {
    if (number != 0x8B || io_count < 1) return 1;
    u32 size;
    const u8* tmd = disc_tmd(&size); /* open partition: hand back the TMD */
    Vec* out = &v[in_count];
    if (out->data) {
        memset(out->data, 0, out->size);
        memcpy(out->data, tmd, size < out->size ? size : out->size);
    }
    if (io_count > 1 && v[in_count + 1].data) memset(v[in_count + 1].data, 0, v[in_count + 1].size);
    return 1;
}

static s32 es_ioctlv(u32 number, Vec* v, u32 in_count, u32 io_count) {
    u32 size;
    const u8* tmd = disc_tmd(&size);
    Vec* out = io_count ? &v[in_count] : NULL;
    for (u32 i = 0; i < io_count; i++)
        if (v[in_count + i].data) memset(v[in_count + i].data, 0, v[in_count + i].size);
    switch (number) {
    case 0x20: /* title ID */
        if (out && out->size >= 8) memcpy(out->data, tmd + 0x18C, 8);
        return 0;
    case 0x1D: { /* title data folder */
        char text[40];
        snprintf(text, sizeof text, "/title/%08x/%08x/data", (u32)load_be(tmd + 0x18C, 4),
                 (u32)load_be(tmd + 0x190, 4));
        if (out) snprintf((char*)out->data, out->size, "%s", text);
        return 0;
    }
    case 0x1B: { /* ticket view: a 4-byte version, then the ticket's fields from ticket ID on */
        u32 ticket_size;
        const u8* ticket = disc_ticket(&ticket_size);
        if (out && ticket_size >= 0x1BC + 0xD4) {
            u32 n = out->size < 0xD4 ? out->size : 0xD4;
            memcpy(out->data + (out->size >= 0xD8 ? 4 : 0), ticket + 0x1D0, n > 0xB8 ? 0xB8 : n);
        }
        return 0;
    }
    default: /* 0x16 consumption and the rest: nothing to report */
        return 0;
    }
}

static s32 fs_ioctl(u32 number, u8* in, u8* out, u32 out_size) {
    char a[65] = {0}, b[65] = {0}, host_a[768], host_b[768];
    switch (number) {
    case 2: /* file system stats */
        if (out && out_size >= 0x1C) {
            static const u32 stats[7] = {0x4000, 0x5DEC, 0x1DD4, 0x10, 0x2F0, 0x146B, 0x394};
            for (int i = 0; i < 7; i++) store_be(out + i * 4, stats[i], 4);
        }
        return 0;
    case 3: /* create folder */
    case 9: /* create file */
        memcpy(a, in + 6, 64);
        if (!host_path(a, host_a, sizeof host_a)) return FS_EINVAL;
        if (exists(host_a)) return FS_EEXIST;
        if (number == 3) return _mkdir(host_a) ? FS_ENOENT : 0;
        {
            FILE* f = fopen(host_a, "wb");
            if (!f) return FS_ENOENT;
            fclose(f);
        }
        return 0;
    case 5: /* set attributes: not stored */
        return 0;
    case 6: /* get attributes */
        memcpy(a, in, 64);
        if (!host_path(a, host_a, sizeof host_a)) return FS_EINVAL;
        if (!exists(host_a)) return FS_ENOENT;
        if (out && out_size >= 0x4C) {
            memset(out, 0, out_size);
            store_be(out + 4, 0x3031, 2);
            memcpy(out + 6, a, 64);
            out[70] = out[71] = out[72] = 3;
        }
        return 0;
    case 7: /* delete */
        memcpy(a, in, 64);
        if (!host_path(a, host_a, sizeof host_a)) return FS_EINVAL;
        if (!exists(host_a)) return FS_ENOENT;
        delete_tree(host_a);
        return 0;
    case 8: /* rename: two 64-byte paths */
        memcpy(a, in, 64);
        memcpy(b, in + 64, 64);
        if (!host_path(a, host_a, sizeof host_a) || !host_path(b, host_b, sizeof host_b))
            return FS_EINVAL;
        if (!exists(host_a)) return FS_ENOENT;
        if (exists(host_b)) delete_tree(host_b);
        return rename(host_a, host_b) ? FS_ENOENT : 0;
    default:
        fprintf(report, "IOS: /dev/fs ioctl %u not handled\n", number);
        return 0;
    }
}

static s32 fs_ioctlv(u32 number, Vec* v, u32 in_count, u32 io_count) {
    char guest[65] = {0}, host[768];
    if (in_count < 1 || !v[0].data) return FS_EINVAL;
    memcpy(guest, v[0].data, 64);
    if (!host_path(guest, host, sizeof host)) return FS_EINVAL;
    if (log_count < MAX_LOG) fprintf(report, "IOS: /dev/fs #%u folder %s\n", number, guest);
    /* The SDK tells files from missing paths by this answer: it lists a path to test it. */
    if (!is_dir(host)) return exists(host) ? FS_EINVAL : FS_ENOENT;
    if (number == 12) { /* usage: clusters and inodes */
        for (u32 i = 0; i < io_count; i++)
            if (v[in_count + i].data) memset(v[in_count + i].data, 0, v[in_count + i].size);
        return 0;
    }
    if (number != 4) {
        fprintf(report, "IOS: /dev/fs ioctlv %u not handled\n", number);
        return 0;
    }
    /* Read folder: one output is just a count; two outputs are 13-byte names and a count. */
    u32 limit = in_count >= 2 && v[1].data ? (u32)load_be(v[1].data, 4) : 0;
    u8* names = io_count >= 2 ? v[in_count].data : NULL;
    u8* count_out = v[in_count + io_count - 1].data;
    u32 count = 0;
    if (names) memset(names, 0, v[in_count].size);
    DIR* d = opendir(host);
    if (d) {
        struct dirent* e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            if (names) {
                if (count == limit) break;
                snprintf((char*)names + count * 13u, 13, "%s", e->d_name);
            }
            count++;
        }
        closedir(d);
    }
    if (count_out) store_be(count_out, count, 4);
    return 0;
}

static s32 file_ioctl(Handle* h, u32 number, u8* out, u32 out_size) {
    if (number != 11 || !out || out_size < 8) return 0; /* 11: size and position */
    long position = ftell(h->file);
    fseek(h->file, 0, SEEK_END);
    long size = ftell(h->file);
    fseek(h->file, position, SEEK_SET);
    store_be(out, (u32)size, 4);
    store_be(out + 4, (u32)position, 4);
    return 0;
}

int ios_handle(u8* r, s32* result) {
    u32 cmd = (u32)load_be(r, 4), fd = (u32)load_be(r + 8, 4);
    u32 a0 = (u32)load_be(r + 12, 4), a1 = (u32)load_be(r + 16, 4), a2 = (u32)load_be(r + 20, 4);
    u32 a3 = (u32)load_be(r + 24, 4), a4 = (u32)load_be(r + 28, 4);
    int log = log_count < MAX_LOG;
    *result = 0;

    if (cmd == 1) {
        char path[64];
        read_guest_string(a0, path, sizeof path);
        *result = open_path(path, a1);
        log_count++;
        if (log) fprintf(report, "IOS open %s mode %u -> %d\n", path, a1, *result);
        return 1;
    }
    Handle* h = fd < MAX_HANDLES && handles[fd].used ? &handles[fd] : NULL;
    if (!h) {
        fprintf(report, "IOS: command %u on bad handle %d\n", cmd, (s32)fd);
        *result = -4;
        return 1;
    }
    u8 *in = NULL, *out = NULL;
    switch (cmd) {
    case 2:
        if (h->file) fclose(h->file);
        h->used = 0;
        break;
    case 3:
    case 4: {
        u8* data = plain_memory(a0, a1);
        if (h->kind != DEV_FILE || !data) {
            *result = FS_EINVAL;
            break;
        }
        if (cmd == 3) {
            *result = (s32)fread(data, 1, a1, h->file);
        } else {
            *result = (s32)fwrite(data, 1, a1, h->file);
            fflush(h->file);
        }
        fseek(h->file, 0, SEEK_CUR); /* required between reads and writes */
        break;
    }
    case 5:
        if (h->kind != DEV_FILE || fseek(h->file, (s32)a0, a1 == 0 ? SEEK_SET : a1 == 1 ? SEEK_CUR : SEEK_END))
            *result = FS_EINVAL;
        else
            *result = (s32)ftell(h->file);
        break;
    case 6:
        in = a2 ? plain_memory(a1, a2) : NULL;
        out = a4 ? plain_memory(a3, a4) : NULL;
        if (h->kind == DEV_EVENTHOOK) { /* stays open until a power or reset event */
            if (log) fprintf(report, "IOS ioctl %s #%u held open\n", h->path, a0);
            return 0;
        }
        if (h->kind == DEV_DI) *result = di_ioctl(a0, in, out, a4);
        else if (h->kind == DEV_FS) *result = fs_ioctl(a0, in, out, a4);
        else if (h->kind == DEV_FILE) *result = file_ioctl(h, a0, out, a4);
        break;
    case 7: {
        Vec v[16] = {{0}};
        u32 total = a1 + a2;
        u8* table = total ? plain_memory(a3, total * 8u) : NULL;
        if (total > 16 || (total && !table)) {
            *result = -4;
            break;
        }
        for (u32 i = 0; i < total; i++) {
            u32 pointer = (u32)load_be(table + i * 8u, 4);
            v[i].size = (u32)load_be(table + i * 8u + 4, 4);
            v[i].data = v[i].size ? plain_memory(pointer, v[i].size) : NULL;
        }
        if (h->kind == DEV_DI) *result = di_ioctlv(a0, v, a1, a2);
        else if (h->kind == DEV_ES) *result = es_ioctlv(a0, v, a1, a2);
        else if (h->kind == DEV_FS) *result = fs_ioctlv(a0, v, a1, a2);
        break;
    }
    default:
        fprintf(report, "IOS: unknown command %u\n", cmd);
        *result = -4;
        break;
    }
    /* Disc reads are too many to list; disc.c logs each file's first read instead. */
    if (!(cmd == 6 && h->kind == DEV_DI && a0 == 0x71)) {
        log_count++;
        if (log) {
            if (cmd == 6) fprintf(report, "IOS ioctl %s #%u -> %d\n", h->path, a0, *result);
            else if (cmd == 7) fprintf(report, "IOS ioctlv %s #%u in %u io %u -> %d\n", h->path, a0, a1, a2, *result);
            else fprintf(report, "IOS cmd %u %s %08X %08X -> %d\n", cmd, h->path, a0, a1, *result);
        }
    }
    return 1;
}
