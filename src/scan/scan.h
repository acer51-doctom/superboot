/*
 * scan.h — Block device scanner interface
 */

#ifndef SUPERBOOT_SCAN_H
#define SUPERBOOT_SCAN_H

#include "../superboot.h"

/*
 * Well-known paths we probe on every partition to detect bootable
 * configurations.  Order matters: first match wins for a given parser.
 */

/* Directories that suggest a Linux /boot or ESP. */
#define SB_PROBE_DIR_BOOT       L"\\boot"
#define SB_PROBE_DIR_EFI        L"\\EFI"
#define SB_PROBE_DIR_LOADER     L"\\loader"

#endif /* SUPERBOOT_SCAN_H */
