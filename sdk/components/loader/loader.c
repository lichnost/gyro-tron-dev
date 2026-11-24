#include "loader/loader.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mmu_map.h"
#include "esp_rom_crc.h"
#include "exports.h"
#include "storage/storage.h"

static const char *TAG = "loader";

#define GAMES_DIR STORAGE_ROOT "/Games"

// ---------------------------------------------------------------- the package

#define TAT_MAGIC "TATPKG\0\x01"
#define KIND_GAME 1

#define SEC_CODE 0x45444F43u   /* 'CODE' little-endian */
#define SEC_ICON 0x4E4F4349u
#define SEC_ASSET 0x54535341u  /* 'ASST' */
#define SEC_SIG 0x20474953u    /* 'SIG ' - skipped at load like any unknown section; its
                                * table entry carries crc 0 by format rule (the signature
                                * is the section's integrity check - a CRC over the
                                * signature bytes would be circular), so every CRC pass
                                * below must skip it too. Verification runs at install
                                * time only (loader_verify_signature, sig.c). */

/* Any other section type is skipped: only these three are read, and the header CRC still
 * covers the rest of the file, so an old package carrying a section nothing reads loads. */

typedef struct __attribute__((packed)) {
    char magic[8];
    uint32_t header_crc32;   /* over everything after this field */
    uint32_t total_size;
    uint8_t kind, reserved;
    uint16_t api_major, api_minor, version;
    char id[16], name[24], author[24];
    uint8_t accent_r, accent_g, accent_b, pad;
    uint16_t section_count;
    uint16_t pad2;
} tat_header_t;

typedef struct __attribute__((packed)) {
    uint32_t type, offset, size, crc32;
} tat_section_t;

// loader_open streams: a multi-MB package next to its own image outgrows the PSRAM left
// for everything else; the scan and the icon read never needed the body (see read_head), and
// loader_inspect is handed an in-memory buffer, so nothing reads whole files any more.

// Everything about a package that its header alone can answer: the magic, the total size,
// the header's own CRC, and that every section lies inside the file. The per-section CRCs
// are deliberately not here: over a 2 MB GB ROM they cost about a second of reading and
// hashing each time, and the scan runs against every installed package several times per
// boot (once for the carousel, once for the icons, again whenever the manager app asks).
// The full check_package runs once, when a game is actually opened - the one place where
// a rejection has a message on screen waiting to catch it.
#define HEAD_MAX (sizeof(tat_header_t) + 64 * sizeof(tat_section_t))

static bool check_head(const uint8_t *buf, size_t have, size_t file_len,
                       const tat_header_t **head_out, const tat_section_t **secs_out)
{
    if (have < sizeof(tat_header_t)) return false;
    const tat_header_t *h = (const tat_header_t *)buf;
    if (memcmp(h->magic, TAT_MAGIC, 8) != 0) return false;
    if (h->total_size != file_len) return false;
    if (h->section_count == 0 || h->section_count > 64) return false;

    const size_t table = sizeof(tat_header_t) + (size_t)h->section_count * sizeof(tat_section_t);
    if (have < table) return false;

    // The header CRC covers everything after the field itself, table included.
    const size_t after = offsetof(tat_header_t, total_size);
    const uint32_t want = esp_rom_crc32_le(0, buf + after, table - after);
    if (want != h->header_crc32) {
        ESP_LOGE(TAG, "header CRC: got %08lx want %08lx", (unsigned long)h->header_crc32,
                 (unsigned long)want);
        return false;
    }

    const tat_section_t *secs = (const tat_section_t *)(buf + sizeof(tat_header_t));
    for (int i = 0; i < h->section_count; i++) {
        if (secs[i].offset < table || secs[i].offset + secs[i].size > file_len) return false;
    }
    *head_out = h;
    *secs_out = secs;
    return true;
}

// Check a package before a byte of it is trusted: everything check_head sees, then every
// section's CRC. A file that arrived half-sent or was edited by hand is rejected here
// rather than crashing the console later.
static bool check_package(const uint8_t *buf, size_t len, const tat_header_t **head_out,
                          const tat_section_t **secs_out)
{
    const tat_header_t *h;
    const tat_section_t *secs;
    if (!check_head(buf, len, len, &h, &secs)) return false;
    for (int i = 0; i < h->section_count; i++) {
        if (secs[i].type == SEC_SIG) continue;   /* crc 0 by format rule, see above */
        if (esp_rom_crc32_le(0, buf + secs[i].offset, secs[i].size) != secs[i].crc32) {
            ESP_LOGE(TAG, "section %d (%.4s) is corrupt", i, (const char *)&secs[i].type);
            return false;
        }
    }
    *head_out = h;
    *secs_out = secs;
    return true;
}

// The header and the section table, read without the body. The scan and the icon lookup
// need nothing else, and a package's body can be the better part of 2 MB. Returns a heap
// buffer to free and the file's full length, or NULL.
static uint8_t *read_head(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < (long)sizeof(tat_header_t)) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = heap_caps_malloc(HEAD_MAX, MALLOC_CAP_SPIRAM);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    const size_t want = (size_t)len < HEAD_MAX ? (size_t)len : HEAD_MAX;
    if (fread(buf, 1, want, f) != want) {
        fclose(f);
        heap_caps_free(buf);
        return NULL;
    }
    fclose(f);
    *len_out = (size_t)len;
    return buf;
}

// One section's bytes, straight from the file. The scan reads only headers, so whatever
// body bytes a caller wants - just the icon, say - come from here, not from memory.
static bool read_span(const char *path, size_t offset, void *dst, size_t len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    const bool ok = fseek(f, (long)offset, SEEK_SET) == 0 && fread(dst, 1, len, f) == len;
    fclose(f);
    return ok;
}

const char *loader_games_dir(void) { return GAMES_DIR; }

uint8_t *loader_read_icon(const char *path, size_t *len_out)
{
    size_t len = 0;
    uint8_t *buf = read_head(path, &len);
    if (!buf) return NULL;

    const tat_header_t *h;
    const tat_section_t *secs;
    uint8_t *icon = NULL;
    if (check_head(buf, len < HEAD_MAX ? len : HEAD_MAX, len, &h, &secs)) {
        for (int i = 0; i < h->section_count && !icon; i++) {
            if (secs[i].type != SEC_ICON || secs[i].size == 0) continue;
            icon = heap_caps_malloc(secs[i].size, MALLOC_CAP_SPIRAM);
            if (!icon) break;
            if (read_span(path, secs[i].offset, icon, secs[i].size)) {
                *len_out = secs[i].size;
            } else {
                heap_caps_free(icon);
                icon = NULL;
            }
        }
    }
    heap_caps_free(buf);
    return icon;
}

void loader_free(void *p) { heap_caps_free(p); }

static void describe(const tat_header_t *h, size_t len, loader_entry_t *o)
{
    memset(o, 0, sizeof(*o));
    snprintf(o->id, sizeof(o->id), "%.15s", h->id);
    snprintf(o->name, sizeof(o->name), "%.23s", h->name);
    snprintf(o->author, sizeof(o->author), "%.23s", h->author);
    o->version = h->version;
    o->api_major = h->api_major;
    o->api_minor = h->api_minor;
    o->accent_r = h->accent_r, o->accent_g = h->accent_g, o->accent_b = h->accent_b;
    o->size = (uint32_t)len;
    o->stamp = h->header_crc32;
    // The launcher shows an unrunnable game rather than hiding it, so that someone whose
    // watch is too old is told why instead of finding the icon simply gone.
    o->runnable = h->api_major == TAT_API_MAJOR && h->api_minor <= TAT_API_MINOR;
}

bool loader_inspect(const uint8_t *buf, size_t len, loader_entry_t *out)
{
    const tat_header_t *h;
    const tat_section_t *secs;
    if (!check_package(buf, len, &h, &secs) || h->kind != KIND_GAME) return false;
    describe(h, len, out);
    return true;
}

int loader_scan(loader_entry_t *out, int max)
{
    DIR *d = opendir(GAMES_DIR);
    if (!d) return 0;   /* nothing installed yet is not an error */

    int n = 0;
    struct dirent *e;
    while (n < max && (e = readdir(d)) != NULL) {
        const size_t nl = strlen(e->d_name);
        if (nl < 5 || strcasecmp(e->d_name + nl - 4, ".tat") != 0) continue;

        char path[sizeof(out[0].path)];
        if ((size_t)snprintf(path, sizeof(path), "%s/%s", GAMES_DIR, e->d_name) >= sizeof(path)) continue;

        size_t len = 0;
        uint8_t *buf = read_head(path, &len);
        if (!buf) continue;
        const tat_header_t *h;
        const tat_section_t *secs;
        if (check_head(buf, len < HEAD_MAX ? len : HEAD_MAX, len, &h, &secs) &&
            h->kind == KIND_GAME) {
            // Two files claiming the same game is a real state to end up in - installing
            // under one name over a copy that arrived under another. The id is what saves
            // and the carousel are keyed on, so the second one is left alone rather than
            // shown twice and given the first one's scores.
            bool seen = false;
            for (int k = 0; k < n && !seen; k++) seen = strcmp(out[k].id, h->id) == 0;
            if (seen) {
                ESP_LOGW(TAG, "%s is already installed; ignoring %s", h->id, e->d_name);
                heap_caps_free(buf);
                continue;
            }
            loader_entry_t *o = &out[n++];
            describe(h, len, o);
            snprintf(o->path, sizeof(o->path), "%s", path);
            ESP_LOGI(TAG, "found %s v%u by %s (%u bytes)%s", o->name, o->version, o->author,
                     (unsigned)o->size, o->runnable ? "" : " - needs a different firmware");
        } else {
            ESP_LOGW(TAG, "%s is not a usable package", e->d_name);
        }
        heap_caps_free(buf);
    }
    closedir(d);
    return n;
}

// ---------------------------------------------------------------- the ELF

typedef struct __attribute__((packed)) {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} Elf32_Ehdr;

typedef struct __attribute__((packed)) {
    uint32_t name, type, flags, addr, offset, size;
    uint32_t link, info, addralign, entsize;
} Elf32_Shdr;

typedef struct __attribute__((packed)) {
    uint32_t name, value, size;
    uint8_t info, other;
    uint16_t shndx;
} Elf32_Sym;

typedef struct __attribute__((packed)) {
    uint32_t offset, info;
    int32_t addend;
} Elf32_Rela;

#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_NOBITS 8
#define SHF_ALLOC 0x2
#define SHF_EXECINSTR 0x4
#define SHN_UNDEF 0
#define ET_EXEC 2
#define EM_XTENSA 94

#define R_XTENSA_NONE 0
#define R_XTENSA_32 1
#define R_XTENSA_ASM_EXPAND 11
#define R_XTENSA_SLOT0_OP 20

// One of the package's asset sections, as a range in the file rather than bytes in
// memory: the bodies are read on demand (see loader_asset_read), because a bundle
// can be megabytes and the console's PSRAM is shared with the game's image and heap.
typedef struct {
    char name[17];      // the section's 16-byte name field, NUL-terminated
    size_t off, len;    // where the body sits in the package file
} asset_ref_t;

struct loader_game {
    void *ram;          // the writable view: where the image was built
    void *exec;         // the executable view of the same physical memory
    size_t span;        // how much was mapped, rounded to MMU pages
    const tat_game_t *desc;
    FILE *file;         // the package, open for the game's whole life: assets stream from it
    uint8_t *head;      // header + section table (kept: asset names live in it)
    asset_ref_t *assets;
    int asset_count;
    // Assets already handed out. They stay for the game's life - a game may keep an
    // asset pointer for a whole run, so eviction is not an option.
    uint8_t **served;
    int served_count;
};

// Every section the linker marked as belonging in memory. They were laid out contiguously
// from zero by the package's linker script, so a section's addr IS its offset in the image.
static size_t image_span(const Elf32_Shdr *sh, int n)
{
    size_t span = 0;
    for (int i = 0; i < n; i++) {
        if (!(sh[i].flags & SHF_ALLOC)) continue;
        const size_t end = sh[i].addr + sh[i].size;
        if (end > span) span = end;
    }
    return span;
}

// `assets` is the table this package's own files live in. A game that ships files declares
// `extern const tat_asset_t tat_assets[]` and points its descriptor at it: built in, the
// firmware build supplies that symbol, and from a package the loader does. It is the one
// name a game may import that the console does not export.
// The image lives at two addresses at once. The same physical memory is mapped normally,
// where it can be read and written a byte at a time, and again on the instruction bus,
// where the processor will execute it - and the instruction bus only does aligned 32-bit
// loads. So a pointer to a function has to carry the executable address and a pointer to
// a string the ordinary one, and which it is depends on the section the symbol lives in.
//
// Getting this wrong is not subtle: point a string at the executable view and the first
// thing that prints it takes a LoadStoreError.
//
// Literal pools are the exception that needs no thought. They sit inside .text and the
// processor reads them with l32r, which is a 32-bit aligned load, so they are fine there.
static uintptr_t base_for(const Elf32_Shdr *sh, uint16_t shndx, uintptr_t data, uintptr_t exec)
{
    return (sh[shndx].flags & SHF_EXECINSTR) ? exec : data;
}

// ---------------------------------------------------------------- streaming reader

// A package read through without ever holding it whole: a multi-MB package next to
// its image does not fit in the PSRAM left after the carousel, so loader_open streams.
// The header, the section table
// and the symbol tables are the only parts kept for the game's life; every section's
// CRC is checked by one dedicated pass over the file (sf_verify). Asset bodies stream
// later still, on api->asset() calls, so multi-MB bundles cost nothing at load time.
#define SF_CHUNK (32 * 1024)
#define RELA_CHUNK 2048   // entries per relocation read, 24 KB

typedef struct {
    FILE *f;
    size_t len;      // the whole file
    size_t cursor;   // the offset the file is positioned at, so back-to-back reads skip the seek
    const tat_header_t *h;         // into the kept header buffer
    const tat_section_t *secs;
} stream_t;

// Read n bytes at off into dst. Sequential ranges continue from the last read's
// position; anything else seeks first.
static int sf_read(stream_t *s, size_t off, void *dst, size_t n)
{
    uint8_t *out = (uint8_t *)dst;
    while (n > 0) {
        const size_t chunk = n > SF_CHUNK ? SF_CHUNK : n;
        if (off != s->cursor) {
            if (fseek(s->f, (long)off, SEEK_SET) != 0) return -1;
        }
        if (off + chunk > s->len || fread(out, 1, chunk, s->f) != chunk) return -1;
        off += chunk;
        out += chunk;
        n -= chunk;
        s->cursor = off;
    }
    return 0;
}

// Every section's CRC, read fresh from the file. The load streams the code section in
// scattered pieces, so its CRC cannot be accumulated on the way past - the unread
// gaps would break the running order. One whole-file pass instead, like the
// resident-copy loader's check_package over the RAM buffer; over flash it costs about
// the same read the first pass took.
static int sf_verify(stream_t *s)
{
    uint8_t *buf = heap_caps_malloc(SF_CHUNK, MALLOC_CAP_SPIRAM);
    if (!buf) return -1;
    int rc = 0;
    const int nsecs = s->h->section_count;
    for (int i = 0; rc == 0 && i < nsecs; i++) {
        const tat_section_t *sec = &s->secs[i];
        if (sec->type == SEC_SIG) continue;   /* crc 0 by format rule, see the section notes */
        uint32_t crc = 0;
        size_t off = sec->offset, left = sec->size;
        while (left > 0) {
            const size_t chunk = left > SF_CHUNK ? SF_CHUNK : left;
            if (fseek(s->f, (long)off, SEEK_SET) != 0 || fread(buf, 1, chunk, s->f) != chunk) {
                rc = -1;
                break;
            }
            crc = esp_rom_crc32_le(crc, buf, chunk);
            off += chunk;
            left -= chunk;
        }
        if (rc == 0 && crc != sec->crc32) {
            ESP_LOGE(TAG, "section %d (%.4s) is corrupt", i, (const char *)&sec->type);
            rc = -1;
        }
    }
    heap_caps_free(buf);
    return rc;
}

// One relocation section's worth of fixups, the entries already read into memory.
// The symbol and string tables ride in `meta`/`syms`/`strs`: they are the one part
// of the ELF the loader needs for its whole pass, so they stay resident while the
// relocations themselves are streamed a section at a time.
static bool apply_rela(const Elf32_Rela *rel, size_t size, const Elf32_Shdr *sh, int shnum,
                       const Elf32_Sym *syms, const char *strs, uint8_t *ram, size_t span,
                       uintptr_t data_base, uintptr_t exec_base, const asset_ref_t *assets)
{
    const int count = (int)(size / sizeof(Elf32_Rela));
    for (int r = 0; r < count; r++) {
        const uint32_t type = rel[r].info & 0xFF;
        // Everything PC-relative inside the image is already correct, because the image
        // is copied in one piece and nothing outside it is reached that way (see
        // docs/reference/GAME_API.md 6.1). ASM_EXPAND is a relaxation hint and we linked with
        // relaxation off, so only the absolute words are left to fix. NONE is the
        // assembler's empty entry, which carries no fixup at all - a link with relaxation
        // off can emit hundreds of them, so the package must load despite them: only the
        // PC emulator links its games and never runs this code.
        if (type == R_XTENSA_NONE || type == R_XTENSA_SLOT0_OP || type == R_XTENSA_ASM_EXPAND)
            continue;
        if (type != R_XTENSA_32) {
            ESP_LOGE(TAG, "relocation type %lu is not supported", (unsigned long)type);
            return false;
        }

        const Elf32_Sym *sym = &syms[rel[r].info >> 8];
        uintptr_t value;
        if (sym->shndx == SHN_UNDEF) {
            const char *name = strs + sym->name;
            // A packaged game's own files: the table the loader built from its ASST
            // sections. Nothing dereferences it in the console (api->asset() goes
            // through the loader), but the game's descriptor points here.
            const void *addr = strcmp(name, "tat_assets") == 0 ? (const void *)assets
                                                               : loader_lookup_export(name);
            if (!addr) {
                ESP_LOGE(TAG, "this game wants '%s', which the console does not export", name);
                return false;
            }
            value = (uintptr_t)addr;
        } else {
            // Linked at zero, so the symbol's value is its offset in the image - and
            // which view that offset is taken from depends on whether it is code.
            value = base_for(sh, sym->shndx, data_base, exec_base) + sym->value;
        }
        // The package is a linked executable, not an object file, so r_offset is
        // already an address in the image rather than an offset within the section.
        // Adding the section's address on top of it - which is what an object file
        // would need - writes wildly past the end and corrupts the heap; .rela.rodata
        // is where the game's descriptor lives, so it does it every single time.
        const uint32_t where = rel[r].offset;
        if (where + 4 > span) {
            ESP_LOGE(TAG, "relocation at %08lx is outside the image", (unsigned long)where);
            return false;
        }
        *(uint32_t *)(ram + where) = (uint32_t)(value + (uintptr_t)rel[r].addend);
    }
    (void)shnum;
    return true;
}

// The game's descriptor, from the already-loaded symbol table.
static const void *find_sym(const Elf32_Sym *syms, int nsyms, const char *strs, const char *want,
                            const Elf32_Shdr *sh, uintptr_t data_base, uintptr_t exec_base)
{
    for (int s = 0; s < nsyms; s++)
        if (syms[s].shndx != SHN_UNDEF && strcmp(strs + syms[s].name, want) == 0)
            return (const void *)(base_for(sh, syms[s].shndx, data_base, exec_base) + syms[s].value);
    return NULL;
}

loader_game_t *loader_open(const char *path)
{
    stream_t st = {0};
    uint8_t *head = NULL;
    Elf32_Shdr *sh = NULL;
    uint8_t *meta = NULL;      // symtab + strtab, for the relocation and descriptor passes
    Elf32_Rela *rela = NULL;   // one relocation section at a time
    loader_game_t *g = NULL;

    st.f = fopen(path, "rb");
    if (!st.f) {
        ESP_LOGE(TAG, "cannot read %s", path);
        return NULL;
    }
    fseek(st.f, 0, SEEK_END);
    const long flen = ftell(st.f);
    fseek(st.f, 0, SEEK_SET);
    if (flen <= 0 || flen > 4 * 1024 * 1024) goto fail;
    st.len = (size_t)flen;

    // Header + section table only; the body streams.
    const size_t want = st.len < HEAD_MAX ? st.len : HEAD_MAX;
    head = heap_caps_malloc(HEAD_MAX, MALLOC_CAP_SPIRAM);
    if (!head || fread(head, 1, want, st.f) != want) goto fail;
    st.cursor = want;
    if (!check_head(head, want, st.len, &st.h, &st.secs) || st.h->kind != KIND_GAME) {
        ESP_LOGE(TAG, "%s is not a usable game package", path);
        goto fail;
    }
    const tat_header_t *h = st.h;
    const tat_section_t *secs = st.secs;

    if (h->api_major != TAT_API_MAJOR || h->api_minor > TAT_API_MINOR) {
        ESP_LOGE(TAG, "%s wants API %u.%u; this console has %u.%u", h->name, h->api_major,
                 h->api_minor, TAT_API_MAJOR, TAT_API_MINOR);
        goto fail;
    }

    const tat_section_t *code = NULL;
    int assets = 0;
    for (int i = 0; i < h->section_count; i++) {
        if (secs[i].type == SEC_CODE) code = &secs[i];
        else if (secs[i].type == SEC_ASSET) assets++;
    }
    if (!code || code->size < sizeof(Elf32_Ehdr)) {
        ESP_LOGE(TAG, "%s has no code", h->name);
        goto fail;
    }

    Elf32_Ehdr eh;
    if (sf_read(&st, code->offset, &eh, sizeof(eh)) != 0 ||
        memcmp(eh.ident, "\x7f" "ELF\1\1", 6) != 0 || eh.machine != EM_XTENSA) {
        ESP_LOGE(TAG, "%s: not an Xtensa ELF", h->name);
        goto fail;
    }
    // It has to be a linked executable: relocation offsets mean something different in an
    // object file, and taking one for the other writes all over memory.
    if (eh.type != ET_EXEC) {
        ESP_LOGE(TAG, "%s: ELF type %u, expected a linked executable", h->name, eh.type);
        goto fail;
    }
    if (eh.shentsize != sizeof(Elf32_Shdr) ||
        eh.shoff + (size_t)eh.shnum * sizeof(Elf32_Shdr) > code->size) {
        ESP_LOGE(TAG, "%s: the section table runs past the end of the code", h->name);
        goto fail;
    }
    sh = heap_caps_malloc((size_t)eh.shnum * sizeof(Elf32_Shdr), MALLOC_CAP_SPIRAM);
    if (!sh || sf_read(&st, code->offset + eh.shoff, sh, (size_t)eh.shnum * sizeof(Elf32_Shdr)) != 0)
        goto fail;

    const size_t span = image_span(sh, eh.shnum);
    if (span == 0 || span > 1024 * 1024) {
        ESP_LOGE(TAG, "%s: image is %u bytes", h->name, (unsigned)span);
        goto fail;
    }

    g = calloc(1, sizeof(*g));
    if (!g) goto fail;
    // st keeps the file for the rest of the load (every read below streams through it);
    // ownership moves to the game only on success, so the fail path can close it.
    g->head = head;
    head = NULL;

    // The asset table is built before the code is relocated, because the game's descriptor
    // points at it and that pointer is one of the things relocation has to fill in.
    if (assets) {
        g->assets = calloc(assets, sizeof(asset_ref_t));
        g->served = calloc(assets, sizeof(uint8_t *));   // one lazily-filled slot per file
        g->served_count = assets;
        if (!g->assets || !g->served) {
            ESP_LOGE(TAG, "no memory for %s's file table", h->name);
            goto fail;
        }
        for (int i = 0; i < h->section_count; i++) {
            if (secs[i].type != SEC_ASSET || secs[i].size < 16) continue;
            // name[16] then the bytes; the name lives at the section's own offset - in the
            // body, which streams and is NOT in the kept head buffer. Reaching into head
            // for it would read far past its HEAD_MAX bytes - garbage that looks like an
            // asset table missing the bundle that is actually there.
            asset_ref_t *a = &g->assets[g->asset_count++];
            if (fseek(st.f, (long)secs[i].offset, SEEK_SET) != 0 ||
                fread(a->name, 1, 16, st.f) != 16) {
                ESP_LOGE(TAG, "%s: cannot read asset %d's name", h->name, i);
                goto fail;
            }
            a->name[16] = 0;
            a->off = secs[i].offset + 16;
            a->len = secs[i].size - 16;
        }
    }

    // Assets are NOT read out here. Holding every bundle whole in PSRAM costs more
    // than the console has and leaves the game short of the very memory the bundle was
    // loaded for. Since api 1.9 a game reads its bundle in chunks (asset_read streams
    // straight from the package file) and never asks for a pointer at all; older games
    // get loader_asset_data's materialize-on-first-ask, which reads through this same
    // handle and keeps the block for the game's life.

    // An MMU page is the unit that can be mapped, so the image gets a whole number of them
    // and has to start on one.
    const size_t kPage = 64 * 1024;
    g->span = (span + kPage - 1) / kPage * kPage;
    g->ram = heap_caps_aligned_alloc(kPage, g->span, MALLOC_CAP_SPIRAM);
    if (!g->ram) {
        ESP_LOGE(TAG, "no memory for %s (%u bytes)", h->name, (unsigned)g->span);
        goto fail;
    }
    memset(g->ram, 0, g->span);

    for (int i = 0; i < eh.shnum; i++) {
        if (!(sh[i].flags & SHF_ALLOC)) continue;
        if (sh[i].type == SHT_NOBITS) continue;   /* .bss: already zeroed above */
        if (sh[i].offset + sh[i].size > code->size || sh[i].addr + sh[i].size > g->span) {
            ESP_LOGE(TAG, "%s: section %d does not fit its own image", h->name, i);
            goto fail;
        }
        if (sf_read(&st, code->offset + sh[i].offset, (uint8_t *)g->ram + sh[i].addr,
                    sh[i].size) != 0)
            goto fail;
    }

    // The code has to be reachable through the instruction bus, and that is a different
    // address for the same physical memory. Map it first, because every pointer inside the
    // image has to be relocated to where the game will actually see itself - then write the
    // relocations through the data view, which is the one that can be written at all.
    esp_paddr_t paddr = 0;
    mmu_target_t target = MMU_TARGET_PSRAM0;
    if (esp_mmu_vaddr_to_paddr(g->ram, &paddr, &target) != ESP_OK) {
        ESP_LOGE(TAG, "cannot find the physical address of the image");
        goto fail;
    }
    esp_err_t err = esp_mmu_map(paddr, g->span, MMU_TARGET_PSRAM0,
                                MMU_MEM_CAP_EXEC | MMU_MEM_CAP_READ, 0, &g->exec);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot map the code to run: %s", esp_err_to_name(err));
        goto fail;
    }
    ESP_LOGI(TAG, "image %u bytes: data view %p, exec view %p", (unsigned)g->span, g->ram, g->exec);

    // The symbol and string tables: the one part of the ELF the loader needs resident
    // while it works (the relocations name their symbols through it).
    const Elf32_Shdr *symtab = NULL;
    for (int i = 0; i < eh.shnum; i++)
        if (sh[i].type == SHT_SYMTAB) symtab = &sh[i];
    if (!symtab || symtab->link >= eh.shnum || sh[symtab->link].type != SHT_STRTAB ||
        symtab->size / sizeof(Elf32_Sym) > 65536) {
        ESP_LOGE(TAG, "%s has no usable symbol table", h->name);
        goto fail;
    }
    const Elf32_Shdr *strtab = &sh[symtab->link];
    meta = heap_caps_malloc(symtab->size + strtab->size, MALLOC_CAP_SPIRAM);
    // Relocations stream in fixed chunks: a game's .rela.text can reach hundreds of KB,
    // and a buffer for the whole section next to its image does not fit in PSRAM.
    rela = heap_caps_malloc(RELA_CHUNK * sizeof(Elf32_Rela), MALLOC_CAP_SPIRAM);
    if (!meta || !rela) {
        ESP_LOGE(TAG, "no memory for %s's symbol table (%u bytes)", h->name,
                 (unsigned)(symtab->size + strtab->size));
        goto fail;
    }
    if (sf_read(&st, code->offset + symtab->offset, meta, symtab->size) != 0 ||
        sf_read(&st, code->offset + strtab->offset, meta + symtab->size, strtab->size) != 0)
        goto fail;
    const Elf32_Sym *syms = (const Elf32_Sym *)meta;
    const char *strs = (const char *)meta + symtab->size;
    const int nsyms = (int)(symtab->size / sizeof(Elf32_Sym));

    for (int i = 0; i < eh.shnum; i++) {
        if (sh[i].type != SHT_RELA) continue;
        // sh_info names the section being patched; sh_link the symbol table to read.
        if (!(sh[sh[i].info].flags & SHF_ALLOC)) continue;
        if (sh[i].link != symtab - sh) {
            ESP_LOGE(TAG, "%s: relocation section %d names a second symbol table", h->name, i);
            goto fail;
        }
        const size_t total = sh[i].size - sh[i].size % sizeof(Elf32_Rela);
        for (size_t done = 0; done < total;) {
            size_t n = total - done;
            if (n > RELA_CHUNK * sizeof(Elf32_Rela)) n = RELA_CHUNK * sizeof(Elf32_Rela);
            if (sf_read(&st, code->offset + sh[i].offset + done, rela, n) != 0) {
                ESP_LOGE(TAG, "%s: cannot read relocation section %d", h->name, i);
                goto fail;
            }
            if (!apply_rela(rela, n, sh, eh.shnum, syms, strs, (uint8_t *)g->ram, g->span,
                            (uintptr_t)g->ram, (uintptr_t)g->exec, g->assets)) {
                goto fail;
            }
            done += n;
        }
    }
    ESP_LOGI(TAG, "relocations applied");

    // Every section's CRC, over a fresh read of the whole file (see sf_verify).
    if (sf_verify(&st) != 0) {
        ESP_LOGE(TAG, "%s failed its CRC check", h->name);
        goto fail;
    }

    // Code written as data has to reach memory before the instruction side reads it.
    esp_cache_msync(g->ram, g->span, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    // The descriptor is const data, so find_sym hands back its address in the data view;
    // the function pointers inside it were relocated to the executable one.
    g->desc = (const tat_game_t *)find_sym(syms, nsyms, strs, "tat_game", sh, (uintptr_t)g->ram,
                                           (uintptr_t)g->exec);
    if (!g->desc || g->desc->magic != TAT_GAME_MAGIC) {
        ESP_LOGE(TAG, "%s exports no game", h->name);
        goto fail;
    }

    ESP_LOGI(TAG, "loaded %s: %u bytes of image at %p, %d assets; psram free %u, largest %u",
             h->name, (unsigned)span, g->exec, g->asset_count,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    g->file = st.f;   // the game owns the file from here; assets stream from it later
    heap_caps_free(sh);
    heap_caps_free(meta);
    heap_caps_free(rela);
    return g;

fail:
    if (st.f) fclose(st.f);
    heap_caps_free(head);
    heap_caps_free(sh);
    heap_caps_free(meta);
    heap_caps_free(rela);
    loader_close(g);
    return NULL;
}

const tat_game_t *loader_descriptor(loader_game_t *g) { return g ? g->desc : NULL; }

int loader_asset_count(loader_game_t *g) { return g ? g->asset_count : 0; }

const char *loader_asset_name(loader_game_t *g, int i)
{
    return g && i >= 0 && i < g->asset_count ? g->assets[i].name : NULL;
}

size_t loader_asset_size(loader_game_t *g, int i)
{
    return g && i >= 0 && i < g->asset_count ? g->assets[i].len : 0;
}

int loader_asset_read(loader_game_t *g, int i, size_t skip, void *dst, size_t n)
{
    if (!g || !g->file || i < 0 || i >= g->asset_count || !dst) return -1;
    const asset_ref_t *a = &g->assets[i];
    if (skip > a->len || n > a->len - skip) return -1;
    if (n == 0) return 0;
    if (fseek(g->file, (long)(a->off + skip), SEEK_SET) != 0) return -1;
    return fread(dst, 1, n, g->file) == n ? (int)n : -1;
}

const void *loader_asset_data(loader_game_t *g, int i, size_t *len)
{
    if (!g || i < 0 || i >= g->asset_count) {
        if (len) *len = 0;
        return NULL;
    }
    if (len) *len = g->assets[i].len;
    if (g->assets[i].len == 0) return "";   // nothing to read; a valid empty asset
    if (i < g->served_count && g->served[i]) return g->served[i];
    uint8_t *buf = heap_caps_malloc(g->assets[i].len, MALLOC_CAP_SPIRAM);
    if (!buf || loader_asset_read(g, i, 0, buf, g->assets[i].len) < 0) {
        heap_caps_free(buf);
        // The game only sees a NULL and blames its own bundle; say what actually happened,
        // and what size of block would have fit - a multi-MB bundle is exactly what the
        // console's leftovers cannot always hold contiguously.
        ESP_LOGE(TAG, "asset %d (%u bytes): %s; psram free %u, largest block %u", i,
                 (unsigned)g->assets[i].len, buf ? "read failed" : "alloc failed",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        if (len) *len = 0;
        return NULL;
    }
    g->served[i] = buf;   // indexed by asset: one slot per file, NULL until first asked
    return buf;
}

void loader_close(loader_game_t *g)
{
    if (!g) return;
    if (g->exec) esp_mmu_unmap(g->exec);
    if (g->ram) heap_caps_free(g->ram);
    if (g->file) fclose(g->file);
    heap_caps_free(g->head);
    if (g->served) {
        for (int i = 0; i < g->served_count; i++) heap_caps_free(g->served[i]);
        free(g->served);
    }
    free(g->assets);
    free(g);
}
