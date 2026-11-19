/* The game's disc, rebuilt from the extracted files. Files are opened read-only.
   Layout follows the public disc format (wiibrew.org/wiki/Wii_disc): boot.bin at 0, bi2 at
   0x440, the apploader at 0x2440, and main.dol, fst.bin and game files where boot.bin and
   fst.bin say. Offsets here are relative to the game partition's data. */
#include <stdlib.h>
#include <string.h>
#include "host.h"

#define MAX_PIECES 2048
#define MAX_READ_LOG 400

typedef struct {
    u64 offset;
    u32 size;
    char path[200]; /* relative to the game folder */
    u64 bytes_read;
} Piece;

static Piece pieces[MAX_PIECES];
static int piece_count;
static char root[512];
static u8 *boot, *fst, *bi2, *tmd, *ticket;
static size_t boot_size, fst_size, bi2_size, tmd_size, ticket_size;
static FILE* open_file;
static int open_piece = -1;
static u32 read_log_count;

static u8* load_whole(const char* relative, size_t* size) {
    char path[768];
    snprintf(path, sizeof path, "%s/%s", root, relative);
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Can't read %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    *size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    u8* data = malloc(*size);
    if (data && fread(data, 1, *size, f) != *size) {
        free(data);
        data = NULL;
    }
    fclose(f);
    return data;
}

static int add_piece(u64 offset, u32 size, const char* path) {
    if (piece_count == MAX_PIECES) return 0;
    Piece* p = &pieces[piece_count++];
    p->offset = offset;
    p->size = size;
    p->bytes_read = 0;
    snprintf(p->path, sizeof p->path, "%s", path);
    return 1;
}

static int by_offset(const void* a, const void* b) {
    u64 x = ((const Piece*)a)->offset, y = ((const Piece*)b)->offset;
    return x < y ? -1 : x > y;
}

/* Each FST entry is 12 bytes: type and name offset, file offset or parent, size or next. */
static int walk_fst(void) {
    u32 count = (u32)load_be(fst + 8, 4);
    const char* names = (const char*)fst + count * 12u;
    char prefix[16][200];
    u32 ends[16];
    int depth = 0;
    strcpy(prefix[0], "files");
    ends[0] = count;
    for (u32 i = 1; i < count; i++) {
        while (depth && i >= ends[depth]) depth--;
        const u8* e = fst + i * 12u;
        const char* name = names + (load_be(e, 4) & 0xFFFFFFu);
        u32 value = (u32)load_be(e + 4, 4), size = (u32)load_be(e + 8, 4);
        char path[200];
        snprintf(path, sizeof path, "%s/%s", prefix[depth], name);
        if (e[0]) { /* a directory: size is the index just past its last entry */
            if (depth + 1 == 16) return 0;
            depth++;
            strcpy(prefix[depth], path);
            ends[depth] = size;
        } else if (!add_piece((u64)value << 2, size, path)) {
            return 0;
        }
    }
    return 1;
}

int disc_open(const char* game) {
    snprintf(root, sizeof root, "%s", game);
    boot = load_whole("sys/boot.bin", &boot_size);
    fst = load_whole("sys/fst.bin", &fst_size);
    bi2 = load_whole("sys/bi2.bin", &bi2_size);
    tmd = load_whole("tmd.bin", &tmd_size);
    ticket = load_whole("ticket.bin", &ticket_size);
    if (!boot || !fst || !bi2 || !tmd || !ticket) return 0;

    size_t apploader_size = 0, dol_size = 0;
    u8* apploader = load_whole("sys/apploader.img", &apploader_size);
    u8* dol = load_whole("sys/main.dol", &dol_size);
    free(apploader);
    free(dol);
    add_piece(0, (u32)boot_size, "sys/boot.bin");
    add_piece(0x440, (u32)bi2_size, "sys/bi2.bin");
    add_piece(0x2440, (u32)apploader_size, "sys/apploader.img");
    add_piece((u64)load_be(boot + 0x420, 4) << 2, (u32)dol_size, "sys/main.dol");
    add_piece((u64)load_be(boot + 0x424, 4) << 2, (u32)fst_size, "sys/fst.bin");
    if (!walk_fst()) return 0;
    qsort(pieces, (size_t)piece_count, sizeof *pieces, by_offset);

    int missing = 0;
    for (int i = 0; i < piece_count; i++) {
        char path[768];
        snprintf(path, sizeof path, "%s/%s", root, pieces[i].path);
        FILE* f = fopen(path, "rb");
        if (f) fclose(f);
        else missing++;
    }
    fprintf(report, "Disc: %d pieces from %s, %d missing\n", piece_count, root, missing);
    return missing == 0;
}

static int find_piece(u64 offset) {
    int low = 0, high = piece_count - 1, found = -1;
    while (low <= high) {
        int mid = (low + high) / 2;
        if (pieces[mid].offset <= offset) {
            found = mid;
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }
    return found;
}

/* Gaps between files read as zero, which is what padding on a disc holds. */
int disc_read(u64 offset, u8* out, u32 size) {
    int ok = 1;
    int logged = 0;
    while (size) {
        int i = find_piece(offset);
        Piece* p = i >= 0 ? &pieces[i] : NULL;
        u32 step;
        if (p && offset < p->offset + p->size) {
            u64 inside = offset - p->offset;
            step = p->size - (u32)inside;
            if (step > size) step = size;
            if (open_piece != i) {
                char path[768];
                if (open_file) fclose(open_file);
                snprintf(path, sizeof path, "%s/%s", root, p->path);
                open_file = fopen(path, "rb");
                open_piece = open_file ? i : -1;
            }
            if (!open_file || _fseeki64(open_file, (long long)inside, SEEK_SET) ||
                fread(out, 1, step, open_file) != step) {
                fprintf(report, "Disc: failed reading %s at %llX\n", p->path,
                        (unsigned long long)inside);
                memset(out, 0, step);
                ok = 0;
            }
            if (!p->bytes_read && !logged && read_log_count < MAX_READ_LOG) {
                read_log_count++;
                logged = 1;
                fprintf(report, "Disc: first read of %s (%u bytes in file)\n", p->path, p->size);
            }
            p->bytes_read += step;
        } else {
            u64 next = i + 1 < piece_count ? pieces[i + 1].offset : offset + size;
            step = next - offset < size ? (u32)(next - offset) : size;
            memset(out, 0, step);
        }
        offset += step;
        out += step;
        size -= step;
    }
    return ok;
}

const u8* disc_header(void) { return boot; }

const u8* disc_fst(u32* size) {
    *size = (u32)fst_size;
    return fst;
}

const u8* disc_bi2(u32* size) {
    *size = (u32)bi2_size;
    return bi2;
}

const u8* disc_tmd(u32* size) {
    *size = (u32)tmd_size;
    return tmd;
}

const u8* disc_ticket(u32* size) {
    *size = (u32)ticket_size;
    return ticket;
}

void disc_write_report(void) {
    int files = 0;
    u64 bytes = 0;
    fprintf(report, "\n[disc] bytes_read size path\n");
    for (int i = 0; i < piece_count; i++) {
        if (!pieces[i].bytes_read) continue;
        files++;
        bytes += pieces[i].bytes_read;
        fprintf(report, "%llu %u %s\n", (unsigned long long)pieces[i].bytes_read, pieces[i].size,
                pieces[i].path);
    }
    fprintf(report, "total %d files, %llu bytes\n", files, (unsigned long long)bytes);
}
