/*
 * linux.c — Linux kernel loader (x86_64 EFI boot protocol)
 *
 * Implements two boot paths:
 *
 *   1. EFI Handover Protocol (preferred)
 *      Modern kernels (>= 3.7, with CONFIG_EFI_STUB) accept a direct
 *      handover from a UEFI application.  We fill in boot_params, keep
 *      boot services alive, and jump to the handover entry point.
 *      The kernel's EFI stub calls ExitBootServices itself.
 *
 *   2. Legacy bzImage Protocol (fallback)
 *      For older kernels: we set up boot_params, load the kernel to
 *      its preferred address, call ExitBootServices ourselves, convert
 *      the EFI memory map to E820, and jump to the 64-bit entry.
 *
 * Both paths handle initrd concatenation (multiple initrds loaded
 * contiguously in memory, sizes summed).
 */

#include "loader.h"
#include "../fs/vfs.h"

/* ------------------------------------------------------------------ */
/*  EFI memory type → E820 type conversion                             */
/* ------------------------------------------------------------------ */

static UINT32
efi_mem_to_e820_type(UINT32 efi_type)
{
    switch (efi_type) {
    case EfiLoaderCode:
    case EfiLoaderData:
    case EfiBootServicesCode:
    case EfiBootServicesData:
    case EfiConventionalMemory:
        return 1; /* E820_RAM */
    case EfiACPIReclaimMemory:
        return 3; /* E820_ACPI */
    case EfiACPIMemoryNVS:
        return 4; /* E820_NVS */
    default:
        return 2; /* E820_RESERVED */
    }
}

UINTN
sb_efi_memmap_to_e820(EFI_MEMORY_DESCRIPTOR *mmap,
                      UINTN mmap_size, UINTN desc_size,
                      E820Entry *e820, UINTN max_entries)
{
    UINTN count = 0;
    UINT8 *p = (UINT8 *)mmap;
    UINT8 *end = p + mmap_size;

    while (p < end && count < max_entries) {
        EFI_MEMORY_DESCRIPTOR *md = (EFI_MEMORY_DESCRIPTOR *)p;
        UINT32 type = efi_mem_to_e820_type(md->Type);
        UINT64 addr = md->PhysicalStart;
        UINT64 size = md->NumberOfPages * 4096;

        /* Merge contiguous regions of the same type. */
        if (count > 0 &&
            e820[count - 1].type == type &&
            e820[count - 1].addr + e820[count - 1].size == addr) {
            e820[count - 1].size += size;
        } else {
            e820[count].addr = addr;
            e820[count].size = size;
            e820[count].type = type;
            count++;
        }

        p += desc_size;
    }

    return count;
}

/* ------------------------------------------------------------------ */
/*  Load initrd(s) into a contiguous memory region                     */
/* ------------------------------------------------------------------ */

static EFI_STATUS
load_initrds(SuperBootContext *ctx, const BootTarget *target,
             EFI_PHYSICAL_ADDRESS *initrd_addr, UINTN *initrd_total)
{
    *initrd_addr  = 0;
    *initrd_total = 0;

    if (target->initrd_count == 0)
        return EFI_SUCCESS;

    /*
     * First pass: determine total size by reading all initrds.
     * We keep the buffers around to avoid double-reading.
     */
    void  *bufs[SB_MAX_INITRDS] = {0};
    UINTN  sizes[SB_MAX_INITRDS] = {0};
    UINTN  total = 0;

    for (UINT32 i = 0; i < target->initrd_count; i++) {
        EFI_STATUS s = sb_vfs_read_file(
            target->device_handle,
            target->initrd_paths[i],
            &bufs[i], &sizes[i]);
        if (EFI_ERROR(s)) {
            SB_LOG(L"WARN: Failed to load initrd %s: %r",
                   target->initrd_paths[i], s);
            continue;
        }
        total += sizes[i];
    }

    if (total == 0)
        return EFI_SUCCESS;

    /* Allocate a single contiguous region for all initrds.
     * Place it below 4 GiB for compatibility with 32-bit fields. */
    UINTN pages = (total + 4095) / 4096;
    EFI_PHYSICAL_ADDRESS addr = 0xFFFFFFFF; /* Below 4 GiB. */
    EFI_STATUS status = ctx->boot_services->AllocatePages(
                            AllocateMaxAddress, EfiLoaderData,
                            pages, &addr);
    if (EFI_ERROR(status)) {
        /* Try anywhere. */
        status = ctx->boot_services->AllocatePages(
                     AllocateAnyPages, EfiLoaderData, pages, &addr);
        if (EFI_ERROR(status))
            goto cleanup;
    }

    /* Copy all initrds contiguously. */
    UINT8 *dest = (UINT8 *)(UINTN)addr;
    for (UINT32 i = 0; i < target->initrd_count; i++) {
        if (bufs[i]) {
            CopyMem(dest, bufs[i], sizes[i]);
            dest += sizes[i];
        }
    }

    *initrd_addr  = addr;
    *initrd_total = total;

cleanup:
    for (UINT32 i = 0; i < target->initrd_count; i++) {
        if (bufs[i])
            FreePool(bufs[i]);
    }
    return status;
}

/* ------------------------------------------------------------------ */
/*  Boot via EFI Handover Protocol                                     */
/* ------------------------------------------------------------------ */

typedef VOID (EFIAPI *LinuxEfiHandover)(
    EFI_HANDLE image, EFI_SYSTEM_TABLE *table,
    LinuxBootParams *params);

static EFI_STATUS
boot_efi_handover(SuperBootContext *ctx, const BootTarget *target,
                  void *kernel_buf, UINTN kernel_size,
                  EFI_PHYSICAL_ADDRESS initrd_addr, UINTN initrd_size)
{
    LinuxSetupHeader *hdr = (LinuxSetupHeader *)
                            ((UINT8 *)kernel_buf + 0x1F1);

    if (hdr->handover_offset == 0)
        return EFI_UNSUPPORTED; /* No handover support. */

    /* Determine setup size. */
    UINTN setup_sects = hdr->setup_sects;
    if (setup_sects == 0) setup_sects = 4;
    UINTN setup_size = (setup_sects + 1) * 512;

    /* Allocate boot_params (zero page). */
    LinuxBootParams *bp = AllocateZeroPool(sizeof(LinuxBootParams));
    if (!bp)
        return EFI_OUT_OF_RESOURCES;

    /* Copy the setup header from the kernel image. */
    CopyMem(&bp->hdr, hdr, sizeof(LinuxSetupHeader));

    /* Set loader identity. */
    bp->hdr.type_of_loader = SUPERBOOT_LOADER_ID;
    bp->hdr.loadflags |= LINUX_CAN_USE_HEAP;
    bp->hdr.heap_end_ptr = 0xFE00;

    /* Command line. */
    UINTN cmdline_len = sb_strlen8(target->cmdline);
    CHAR8 *cmdline = AllocatePool(cmdline_len + 1);
    if (cmdline) {
        CopyMem(cmdline, (void *)target->cmdline, cmdline_len + 1);
        bp->hdr.cmd_line_ptr = (UINT32)(UINTN)cmdline;
    }

    /* Initrd. */
    bp->hdr.ramdisk_image = (UINT32)initrd_addr;
    bp->hdr.ramdisk_size  = (UINT32)initrd_size;

    /* Compute the handover entry point.
     * For 64-bit: kernel_base + 512 + handover_offset */
    UINT8 *kernel_base = (UINT8 *)kernel_buf + setup_size;
    LinuxEfiHandover handover = (LinuxEfiHandover)(
        kernel_base + hdr->handover_offset + 512);

    SB_LOG(L"Jumping to kernel via EFI handover at %p", handover);

    /* The handover protocol does NOT return. */
    handover(ctx->image_handle, ctx->system_table, bp);

    /* Should never reach here. */
    return EFI_LOAD_ERROR;
}

/* ------------------------------------------------------------------ */
/*  Boot via legacy bzImage protocol (ExitBootServices path)           */
/* ------------------------------------------------------------------ */

typedef VOID (*LinuxEntry64)(LinuxBootParams *bp, VOID *unused);

static EFI_STATUS
boot_legacy_bzimage(SuperBootContext *ctx, const BootTarget *target,
                    void *kernel_buf, UINTN kernel_size,
                    EFI_PHYSICAL_ADDRESS initrd_addr, UINTN initrd_size)
{
    LinuxSetupHeader *hdr = (LinuxSetupHeader *)
                            ((UINT8 *)kernel_buf + 0x1F1);

    UINTN setup_sects = hdr->setup_sects;
    if (setup_sects == 0) setup_sects = 4;
    UINTN setup_size = (setup_sects + 1) * 512;
    UINTN kernel_raw_size = kernel_size - setup_size;

    /* Allocate boot_params. */
    LinuxBootParams *bp = AllocateZeroPool(sizeof(LinuxBootParams));
    if (!bp)
        return EFI_OUT_OF_RESOURCES;

    CopyMem(&bp->hdr, hdr, sizeof(LinuxSetupHeader));
    bp->hdr.type_of_loader = SUPERBOOT_LOADER_ID;
    bp->hdr.loadflags |= LINUX_CAN_USE_HEAP;
    bp->hdr.heap_end_ptr = 0xFE00;

    /* Copy kernel protected-mode code to preferred address. */
    EFI_PHYSICAL_ADDRESS kernel_addr = hdr->pref_address;
    if (kernel_addr == 0)
        kernel_addr = 0x100000; /* 1 MiB default. */

    UINTN kernel_pages = (kernel_raw_size + 4095) / 4096;
    EFI_STATUS status = ctx->boot_services->AllocatePages(
                            AllocateAddress, EfiLoaderData,
                            kernel_pages, &kernel_addr);
    if (EFI_ERROR(status)) {
        /* If preferred address is taken, try anywhere (relocatable). */
        if (hdr->relocatable_kernel) {
            status = ctx->boot_services->AllocatePages(
                         AllocateAnyPages, EfiLoaderData,
                         kernel_pages, &kernel_addr);
            if (EFI_ERROR(status))
                return status;
        } else {
            return status;
        }
    }

    CopyMem((void *)(UINTN)kernel_addr,
            (UINT8 *)kernel_buf + setup_size, kernel_raw_size);

    bp->hdr.code32_start = (UINT32)kernel_addr;

    /* Command line. */
    UINTN cmdline_len = sb_strlen8(target->cmdline);
    CHAR8 *cmdline = AllocatePool(cmdline_len + 1);
    if (cmdline) {
        CopyMem(cmdline, (void *)target->cmdline, cmdline_len + 1);
        bp->hdr.cmd_line_ptr = (UINT32)(UINTN)cmdline;
    }

    /* Initrd. */
    bp->hdr.ramdisk_image = (UINT32)initrd_addr;
    bp->hdr.ramdisk_size  = (UINT32)initrd_size;

    /*
     * Get the UEFI memory map and exit boot services.
     *
     * This is the critical hand-off point.  After ExitBootServices:
     *   - No more UEFI boot service calls allowed
     *   - We must not allocate memory
     *   - We must jump to the kernel immediately
     *
     * GetMemoryMap + ExitBootServices must be called in a tight loop
     * because any intervening allocation invalidates the map key.
     */
    UINTN  mmap_size = 0, map_key, desc_size;
    UINT32 desc_version;
    EFI_MEMORY_DESCRIPTOR *mmap = NULL;

    /* First call: get required buffer size. */
    ctx->boot_services->GetMemoryMap(
        &mmap_size, NULL, &map_key, &desc_size, &desc_version);

    /* Add slack for the allocation itself. */
    mmap_size += desc_size * 4;
    mmap = AllocatePool(mmap_size);
    if (!mmap)
        return EFI_OUT_OF_RESOURCES;

    status = ctx->boot_services->GetMemoryMap(
                 &mmap_size, mmap, &map_key, &desc_size, &desc_version);
    if (EFI_ERROR(status))
        return status;

    /* Convert EFI memory map to E820 for the kernel. */
    E820Entry e820[128];
    UINTN e820_count = sb_efi_memmap_to_e820(
                           mmap, mmap_size, desc_size, e820, 128);
    bp->e820_entries = (UINT8)e820_count;
    /* E820 table lives at offset 0x2D0 in boot_params. */
    CopyMem((UINT8 *)bp + 0x2D0, e820,
            e820_count * sizeof(E820Entry));

    /* Exit boot services.  If this fails (map key stale), retry once. */
    status = ctx->boot_services->ExitBootServices(
                 ctx->image_handle, map_key);
    if (EFI_ERROR(status)) {
        /* Re-fetch the map and try again. */
        mmap_size = 0;
        ctx->boot_services->GetMemoryMap(
            &mmap_size, NULL, &map_key, &desc_size, &desc_version);
        mmap_size += desc_size * 4;
        /* Reuse existing buffer if large enough — no allocation. */
        status = ctx->boot_services->GetMemoryMap(
                     &mmap_size, mmap, &map_key, &desc_size, &desc_version);
        if (!EFI_ERROR(status))
            status = ctx->boot_services->ExitBootServices(
                         ctx->image_handle, map_key);
        if (EFI_ERROR(status))
            return status;
    }

    /* === POINT OF NO RETURN ===
     * Boot services are gone.  No Print(), no Allocate, nothing.
     * Jump to the 64-bit kernel entry point. */

    LinuxEntry64 entry = (LinuxEntry64)(UINTN)kernel_addr;
    entry(bp, NULL);

    /* Never reached. */
    return EFI_LOAD_ERROR;
}

/* ------------------------------------------------------------------ */
/*  Public API: sb_boot_linux                                          */
/* ------------------------------------------------------------------ */

EFI_STATUS
sb_boot_linux(SuperBootContext *ctx, const BootTarget *target)
{
    EFI_STATUS status;
    void  *kernel_buf  = NULL;
    UINTN  kernel_size = 0;

    /* Load the kernel image into memory. */
    SB_LOG(L"Loading kernel: %s", target->kernel_path);
    status = sb_vfs_read_file(target->device_handle,
                              target->kernel_path,
                              &kernel_buf, &kernel_size);
    SB_CHECK(status, L"Failed to load kernel");

    /* Validate the setup header. */
    if (kernel_size < 0x260) {
        SB_LOG(L"Kernel image too small (%u bytes)", kernel_size);
        FreePool(kernel_buf);
        return EFI_INVALID_PARAMETER;
    }

    LinuxSetupHeader *hdr = (LinuxSetupHeader *)
                            ((UINT8 *)kernel_buf + 0x1F1);

    if (hdr->header != LINUX_BOOT_HDR_MAGIC) {
        SB_LOG(L"Invalid kernel magic (expected HdrS, got 0x%08x)",
               hdr->header);
        FreePool(kernel_buf);
        return EFI_INVALID_PARAMETER;
    }

    SB_LOG(L"Kernel boot protocol version: %d.%02d",
           hdr->version >> 8, hdr->version & 0xFF);

    /* Load initrds. */
    EFI_PHYSICAL_ADDRESS initrd_addr = 0;
    UINTN initrd_size = 0;
    status = load_initrds(ctx, target, &initrd_addr, &initrd_size);
    if (EFI_ERROR(status))
        SB_LOG(L"WARN: initrd load failed: %r (continuing without)", status);

    if (initrd_size > 0)
        SB_LOG(L"Initrd: %u bytes at 0x%lx", initrd_size, initrd_addr);

    SB_LOG(L"Cmdline: %a", target->cmdline);

    /* Prefer EFI handover if available (keeps boot services alive
     * so the kernel's EFI stub can use them). */
    if (hdr->version >= 0x020B && hdr->handover_offset != 0) {
        SB_LOG(L"Using EFI handover protocol");
        status = boot_efi_handover(ctx, target, kernel_buf, kernel_size,
                                   initrd_addr, initrd_size);
        /* If handover fails, fall through to legacy path. */
        if (status != EFI_UNSUPPORTED)
            return status;
    }

    /* Fallback: legacy bzImage boot. */
    SB_LOG(L"Using legacy bzImage boot protocol");
    return boot_legacy_bzimage(ctx, target, kernel_buf, kernel_size,
                               initrd_addr, initrd_size);
}
