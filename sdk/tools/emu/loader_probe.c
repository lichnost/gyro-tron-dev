// The main() of the emulator's `load` verb: it runs the REAL package loader
// (components/loader/loader.c) against a .tat on the PC. The play/shot paths link a game
// straight into the emulator and never touch the loader, so a package the watch refuses
// for a loader-level reason (a relocation type, a missing export) loads invisibly there -
// this probe is the only host-side check that sees what a watch would log and show as
// THIS GAME WOULD NOT LOAD.
#include <stdio.h>

#include "loader/loader.h"

// The xtensa compiler helpers exports.c only takes addresses of do not exist on the host -
// dummy bodies so the table can be built; nothing here calls them.
void __divsf3(void) {} void __divdi3(void) {} void __udivdi3(void) {} void __moddi3(void) {}
void __umoddi3(void) {} void __extendsfdf2(void) {} void __truncdfsf2(void) {}
void __floatdisf(void) {} void __fixsfsi(void) {} void __floatsisf(void) {}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <file.tat>\n", argv[0]); return 2; }
    loader_game_t *g = loader_open(argv[1]);
    if (!g) {
        // The reason is already on stderr as an E line: that is the watch's own log line.
        fprintf(stderr, "\nRESULT: the watch would reject this package (THIS GAME WOULD NOT LOAD)\n");
        return 1;
    }
    const tat_game_t *d = loader_descriptor(g);
    printf("\nRESULT: loaded - the watch would run this package\n");
    printf("  descriptor %p, magic %08x\n", (const void *)d, d ? d->magic : 0);
    int n = loader_asset_count(g);
    for (int i = 0; i < n; i++)
        printf("  asset %d: %s (%d bytes)\n", i, loader_asset_name(g, i),
               (int)loader_asset_size(g, i));
    // What the game's first api->asset() does: stream the whole body into PSRAM. A
    // multi-MB bundle can fail right here on a watch whose carousel already ate most of
    // it - the game would see a NULL where its asset should be, so probe it, not just
    // the name table.
    for (int i = 0; i < n; i++) {
        size_t len = 0;
        const void *data = loader_asset_data(g, i, &len);
        printf("  asset %d body: %s (%u bytes)\n", i, data ? "served" : "NOT SERVED",
               (unsigned)len);
        if (!data) {
            fprintf(stderr, "RESULT: the watch would load but starve this game's assets\n");
            return 1;
        }
    }
    return 0;
}
