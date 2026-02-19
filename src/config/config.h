/*
 * config.h — Config parser registry and interface
 *
 * Each supported bootloader format (GRUB, systemd-boot, Limine) is a
 * ConfigParser.  Parsers are stateless: they receive raw file contents
 * and return an array of BootTargets.
 *
 * The scanner (scan.c) feeds config files to sb_parse_configs(), which
 * dispatches to every registered parser in turn.
 */

#ifndef SUPERBOOT_CONFIG_H
#define SUPERBOOT_CONFIG_H

#include "../superboot.h"

/* ------------------------------------------------------------------ */
/*  Config-parser vtable                                               */
/* ------------------------------------------------------------------ */

typedef struct ConfigParser {
    const CHAR16 *name;            /* e.g. L"GRUB"                    */
    ConfigType    type;

    /*
     * config_paths — NULL-terminated list of paths to probe on a given
     * partition.  The scanner opens each in order; the first hit wins.
     *
     * Paths are relative to the filesystem root of the partition,
     * using backslash as separator (UEFI convention).
     */
    const CHAR16 **config_paths;

    /*
     * parse() — turn raw config text into BootTargets.
     *
     *  config_data / config_size   NUL-terminated ASCII text.
     *  device                      UEFI handle of the source partition.
     *  targets / count / max       Output array (caller-owned).
     *
     * Returns EFI_SUCCESS even if zero entries are found (count == 0).
     * Returns an error only on hard failures (OOM, corrupt data, etc.)
     */
    EFI_STATUS (*parse)(
        const CHAR8    *config_data,
        UINTN           config_size,
        EFI_HANDLE      device,
        const CHAR16   *config_path,
        BootTarget     *targets,
        UINTN          *count,
        UINTN           max
    );
} ConfigParser;

/* ------------------------------------------------------------------ */
/*  Registration                                                       */
/* ------------------------------------------------------------------ */

/*
 * Built-in parsers.  Each .c file exposes a single global instance
 * that is referenced by the parser table in config.c.
 */
extern ConfigParser sb_parser_grub;
extern ConfigParser sb_parser_systemd_boot;
extern ConfigParser sb_parser_limine;

/*
 * sb_config_get_parsers() — return the NULL-terminated parser array.
 */
const ConfigParser **sb_config_get_parsers(void);

/* ------------------------------------------------------------------ */
/*  GRUB variable table (shared between grub.c and the transpiler)     */
/* ------------------------------------------------------------------ */

typedef struct {
    CHAR8   name[SB_MAX_VAR_NAME];
    CHAR8   value[SB_MAX_VAR_VALUE];
} GrubVar;

typedef struct {
    GrubVar entries[SB_MAX_VARS];
    UINTN   count;
} GrubVarTable;

void        grub_var_set(GrubVarTable *t, const CHAR8 *name, const CHAR8 *val);
const CHAR8 *grub_var_get(const GrubVarTable *t, const CHAR8 *name);
UINTN       grub_var_expand(const GrubVarTable *t,
                            const CHAR8 *src, CHAR8 *dst, UINTN max);

#endif /* SUPERBOOT_CONFIG_H */
