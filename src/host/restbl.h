/* [UIT 0042] The resource size table (System/Resource/ResourceSizeTable.Product.<ver>.rsizetable.zs) with mods.
 * The game reserves memory for each resource from this table, so a mod file bigger than the game's own, or a file
 * the game does not have, needs a bigger or a new entry. With mods on, the game is served a merge made at start:
 *   - the game's table;
 *   - every entry of the tables mods ship (all of them, not only the winner's: the larger size of each entry);
 *   - each mod file: the game's entry plus how much bigger the mod file is (decoded), or an estimate for a new file.
 * Sizes only ever go up. Format "RESTBL" v1: a 0x16 header (magic, version, name length, counts), {crc32, size}
 * pairs sorted by crc32, then {char name[len], size} entries for names whose crc32 is shared by another name.
 * Our own code and estimates (the format is documented by the modding community). */
#ifndef UIT_RESTBL_H
#define UIT_RESTBL_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uint32_t restbl_crc32(const char *s);
/* A file's decoded size from its first bytes (n >= 32 is plenty): a zstd frame's content size, a .mc file's header,
   else the size itself. 0: a zstd frame that does not say (estimated from the file size). */
uint64_t restbl_decoded_size(const char *path, const uint8_t *head, size_t n, uint64_t file_size);
/* What the game needs for a file of this table name and decoded size (an upper estimate). */
uint64_t restbl_estimate(const char *name, uint64_t decoded);
typedef struct {
    const char *name;        /* table name (modfs_restbl_name) */
    uint64_t mod_decoded;    /* restbl_decoded_size of the mod's file */
    uint64_t game_decoded;   /* of the game's file; 0 when the game has no such file */
} RestblFile;
typedef struct {
    int tables, tables_bad;  /* mod tables merged / not readable */
    int from_tables;         /* entries raised or added by mod tables */
    int raised, added;       /* entries raised / added for mod files */
    int no_entry;            /* game files with no entry (the game does not need one) */
    uint32_t entries;        /* entries in the result */
} RestblStats;
/* game_zs: the game's table (zstd). tables: the mods' tables (zstd), highest mod first. On success *out is a new
   zstd table (free() it) and 0 is returned; 1 = nothing to change (*out NULL); <0 = the game's table is unreadable. */
int restbl_merge(const uint8_t *game_zs, size_t game_n, const uint8_t *const *tables, const size_t *table_n, int ntables,
                 const RestblFile *files, size_t nfiles, uint8_t **out, size_t *out_n, RestblStats *st);
#ifdef __cplusplus
}
#endif
#endif
