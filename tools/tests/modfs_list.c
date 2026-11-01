/* Prints what modfs.c makes of a mods folder: one line per mod, "<+|-><name>\t<files>\t<flags>" (test_mods.py checks
 * that the game reads the order and on/off tools/mods.py wrote).  cc -Isrc/host tools/tests/modfs_list.c src/host/modfs.c */
#include <stdio.h>
#include "modfs.h"
int main(int argc, char **argv) {
    Modfs m;
    if (argc < 2) return 2;
    modfs_load(&m, argv[1], 1, NULL, NULL, NULL);
    for (int i = 0; i < m.nmods; i++)
        printf("%c%s\t%d\t%u\n", m.mods[i].enabled ? '+' : '-', m.mods[i].name, m.mods[i].nfiles, m.mods[i].unsupported);
    modfs_free(&m);
    return 0;
}
