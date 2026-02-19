/*
 * memory.c — Memory allocation wrappers
 */

#include "util.h"

void *
sb_alloc(EFI_BOOT_SERVICES *bs, UINTN size)
{
    void *ptr = NULL;
    EFI_STATUS s = bs->AllocatePool(EfiLoaderData, size, &ptr);
    if (EFI_ERROR(s))
        return NULL;
    SetMem(ptr, size, 0);
    return ptr;
}

void *
sb_alloc_pages(EFI_BOOT_SERVICES *bs, UINTN pages,
               EFI_PHYSICAL_ADDRESS preferred)
{
    EFI_PHYSICAL_ADDRESS addr = preferred;
    EFI_STATUS s;

    if (preferred != 0) {
        s = bs->AllocatePages(AllocateAddress, EfiLoaderData, pages, &addr);
        if (!EFI_ERROR(s))
            return (void *)(UINTN)addr;
    }

    s = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &addr);
    if (EFI_ERROR(s))
        return NULL;
    return (void *)(UINTN)addr;
}

void
sb_free(EFI_BOOT_SERVICES *bs, void *p, UINTN size)
{
    (void)size;
    if (p)
        bs->FreePool(p);
}

void
sb_free_pages(EFI_BOOT_SERVICES *bs, EFI_PHYSICAL_ADDRESS addr, UINTN pages)
{
    bs->FreePages(addr, pages);
}
