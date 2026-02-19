/*
 * ext4.c — Read-only ext4 filesystem driver
 *
 * Supports ext2/ext3/ext4 with the following limitations:
 *   - Read-only (no write, no journal replay)
 *   - No encryption (fscrypt)
 *   - No inline data (for very small files)
 *   - Extent-based files only (ext4 default since Linux 2.6.23)
 *
 * The driver reads the superblock to identify the filesystem,
 * then navigates the inode table and extent tree to read files.
 */

#include "vfs.h"

/* ------------------------------------------------------------------ */
/*  ext4 on-disk structures                                            */
/* ------------------------------------------------------------------ */

#define EXT4_SUPER_MAGIC        0xEF53
#define EXT4_SUPERBLOCK_OFFSET  1024
#define EXT4_ROOT_INO           2

#pragma pack(1)

typedef struct {
    UINT32 s_inodes_count;
    UINT32 s_blocks_count_lo;
    UINT32 s_r_blocks_count_lo;
    UINT32 s_free_blocks_count_lo;
    UINT32 s_free_inodes_count;
    UINT32 s_first_data_block;
    UINT32 s_log_block_size;       /* Block size = 1024 << this */
    UINT32 s_log_cluster_size;
    UINT32 s_blocks_per_group;
    UINT32 s_clusters_per_group;
    UINT32 s_inodes_per_group;
    UINT32 s_mtime;
    UINT32 s_wtime;
    UINT16 s_mnt_count;
    UINT16 s_max_mnt_count;
    UINT16 s_magic;                /* Must be 0xEF53 */
    UINT16 s_state;
    UINT16 s_errors;
    UINT16 s_minor_rev_level;
    UINT32 s_lastcheck;
    UINT32 s_checkinterval;
    UINT32 s_creator_os;
    UINT32 s_rev_level;
    UINT16 s_def_resuid;
    UINT16 s_def_resgid;
    /* Extended superblock fields (rev >= 1). */
    UINT32 s_first_ino;
    UINT16 s_inode_size;
    UINT16 s_block_group_nr;
    UINT32 s_feature_compat;
    UINT32 s_feature_incompat;
    UINT32 s_feature_ro_compat;
    UINT8  s_uuid[16];
    CHAR8  s_volume_name[16];
    CHAR8  s_last_mounted[64];
    UINT32 s_algorithm_usage_bitmap;
    /* ... additional fields omitted for brevity */
} Ext4Superblock;

typedef struct {
    UINT32 bg_block_bitmap_lo;
    UINT32 bg_inode_bitmap_lo;
    UINT32 bg_inode_table_lo;
    UINT16 bg_free_blocks_count_lo;
    UINT16 bg_free_inodes_count_lo;
    UINT16 bg_used_dirs_count_lo;
    UINT16 bg_flags;
    UINT32 bg_exclude_bitmap_lo;
    UINT16 bg_block_bitmap_csum_lo;
    UINT16 bg_inode_bitmap_csum_lo;
    UINT16 bg_itable_unused_lo;
    UINT16 bg_checksum;
    /* 64-bit fields follow for 64-bit mode. */
} Ext4GroupDesc;

typedef struct {
    UINT16 i_mode;
    UINT16 i_uid;
    UINT32 i_size_lo;
    UINT32 i_atime;
    UINT32 i_ctime;
    UINT32 i_mtime;
    UINT32 i_dtime;
    UINT16 i_gid;
    UINT16 i_links_count;
    UINT32 i_blocks_lo;
    UINT32 i_flags;
    UINT32 i_osd1;
    UINT8  i_block[60];           /* Extent tree or block pointers */
    UINT32 i_generation;
    UINT32 i_file_acl_lo;
    UINT32 i_size_high;
    UINT32 i_obso_faddr;
    UINT8  i_osd2[12];
    UINT16 i_extra_isize;
    UINT16 i_checksum_hi;
    UINT32 i_ctime_extra;
    UINT32 i_mtime_extra;
    UINT32 i_atime_extra;
    UINT32 i_crtime;
    UINT32 i_crtime_extra;
    UINT32 i_version_hi;
    UINT32 i_projid;
} Ext4Inode;

/* Extent tree header. */
typedef struct {
    UINT16 eh_magic;              /* 0xF30A */
    UINT16 eh_entries;
    UINT16 eh_max;
    UINT16 eh_depth;
    UINT32 eh_generation;
} Ext4ExtentHeader;

/* Leaf extent. */
typedef struct {
    UINT32 ee_block;              /* Logical block number */
    UINT16 ee_len;
    UINT16 ee_start_hi;
    UINT32 ee_start_lo;           /* Physical block number */
} Ext4Extent;

/* Index extent (for depth > 0). */
typedef struct {
    UINT32 ei_block;
    UINT32 ei_leaf_lo;
    UINT16 ei_leaf_hi;
    UINT16 ei_unused;
} Ext4ExtentIdx;

/* Directory entry. */
typedef struct {
    UINT32 inode;
    UINT16 rec_len;
    UINT8  name_len;
    UINT8  file_type;
    CHAR8  name[];                /* Variable length, NOT NUL-terminated */
} Ext4DirEntry2;

#pragma pack()

/* File types in dir entries. */
#define EXT4_FT_REG_FILE  1
#define EXT4_FT_DIR       2

/* Inode flags. */
#define EXT4_EXTENTS_FL   0x00080000

/* ------------------------------------------------------------------ */
/*  Driver context                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    EFI_BLOCK_IO_PROTOCOL *block_io;
    EFI_DISK_IO_PROTOCOL  *disk_io;
    Ext4Superblock         sb;
    UINT32                 block_size;
    UINT32                 inode_size;
    UINT32                 group_desc_size;
} Ext4Context;

/* ------------------------------------------------------------------ */
/*  Block I/O helpers                                                  */
/* ------------------------------------------------------------------ */

static EFI_STATUS
ext4_read_block(Ext4Context *c, UINT64 block, void *buf)
{
    UINT64 offset = block * c->block_size;
    if (c->disk_io) {
        return c->disk_io->ReadDisk(
            c->disk_io, c->block_io->Media->MediaId,
            offset, c->block_size, buf);
    }
    /* Fallback: use Block I/O with aligned reads. */
    UINT64 lba = offset / c->block_io->Media->BlockSize;
    return c->block_io->ReadBlocks(
        c->block_io, c->block_io->Media->MediaId,
        lba, c->block_size, buf);
}

static EFI_STATUS
ext4_read_bytes(Ext4Context *c, UINT64 offset, UINTN size, void *buf)
{
    if (c->disk_io) {
        return c->disk_io->ReadDisk(
            c->disk_io, c->block_io->Media->MediaId,
            offset, size, buf);
    }
    /* Block I/O fallback — read full blocks containing the range. */
    UINT32 bs = c->block_io->Media->BlockSize;
    UINT64 start_lba = offset / bs;
    UINT64 end_lba = (offset + size + bs - 1) / bs;
    UINTN  total = (UINTN)(end_lba - start_lba) * bs;

    void *tmp = AllocatePool(total);
    if (!tmp)
        return EFI_OUT_OF_RESOURCES;

    EFI_STATUS s = c->block_io->ReadBlocks(
        c->block_io, c->block_io->Media->MediaId,
        start_lba, total, tmp);
    if (!EFI_ERROR(s))
        CopyMem(buf, (UINT8 *)tmp + (offset % bs), size);
    FreePool(tmp);
    return s;
}

/* ------------------------------------------------------------------ */
/*  Inode lookup                                                       */
/* ------------------------------------------------------------------ */

static EFI_STATUS
ext4_read_inode(Ext4Context *c, UINT32 ino, Ext4Inode *inode)
{
    UINT32 group = (ino - 1) / c->sb.s_inodes_per_group;
    UINT32 index = (ino - 1) % c->sb.s_inodes_per_group;

    /* Read group descriptor. */
    UINT64 gd_offset = (UINT64)(c->sb.s_first_data_block + 1) * c->block_size
                       + (UINT64)group * c->group_desc_size;

    Ext4GroupDesc gd;
    EFI_STATUS s = ext4_read_bytes(c, gd_offset, sizeof(gd), &gd);
    if (EFI_ERROR(s))
        return s;

    /* Read the inode from the inode table. */
    UINT64 inode_offset = (UINT64)gd.bg_inode_table_lo * c->block_size
                          + (UINT64)index * c->inode_size;

    return ext4_read_bytes(c, inode_offset, sizeof(Ext4Inode), inode);
}

/* ------------------------------------------------------------------ */
/*  Extent tree traversal → read file data                             */
/* ------------------------------------------------------------------ */

static EFI_STATUS
ext4_read_file_data(Ext4Context *c, Ext4Inode *inode,
                    void *buf, UINT64 file_size)
{
    if (!(inode->i_flags & EXT4_EXTENTS_FL))
        return EFI_UNSUPPORTED; /* Only extent-based files. */

    Ext4ExtentHeader *eh = (Ext4ExtentHeader *)inode->i_block;
    if (eh->eh_magic != 0xF30A)
        return EFI_VOLUME_CORRUPTED;

    if (eh->eh_depth != 0)
        return EFI_UNSUPPORTED; /* TODO: handle index nodes. */

    Ext4Extent *ext = (Ext4Extent *)(eh + 1);
    UINT8 *dst = (UINT8 *)buf;
    UINT64 remaining = file_size;

    for (UINT16 i = 0; i < eh->eh_entries && remaining > 0; i++) {
        UINT64 phys_block = ((UINT64)ext[i].ee_start_hi << 32)
                            | ext[i].ee_start_lo;
        UINT32 len_blocks = ext[i].ee_len;
        /* High bit set means uninitialized (sparse). */
        if (len_blocks > 32768) len_blocks -= 32768;

        for (UINT32 b = 0; b < len_blocks && remaining > 0; b++) {
            UINTN to_read = (remaining < c->block_size)
                            ? (UINTN)remaining : c->block_size;
            EFI_STATUS s = ext4_read_block(c, phys_block + b, dst);
            if (EFI_ERROR(s))
                return s;
            dst += to_read;
            remaining -= to_read;
        }
    }

    return EFI_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  Directory lookup: find an entry by name                            */
/* ------------------------------------------------------------------ */

static UINT32
ext4_dir_lookup(Ext4Context *c, Ext4Inode *dir_inode, const CHAR8 *name)
{
    UINT64 dir_size = ((UINT64)dir_inode->i_size_high << 32)
                      | dir_inode->i_size_lo;

    void *dir_data = AllocatePool((UINTN)dir_size);
    if (!dir_data)
        return 0;

    if (EFI_ERROR(ext4_read_file_data(c, dir_inode, dir_data, dir_size))) {
        FreePool(dir_data);
        return 0;
    }

    UINTN name_len = sb_strlen8(name);
    UINT8 *p = (UINT8 *)dir_data;
    UINT8 *end = p + (UINTN)dir_size;
    UINT32 result = 0;

    while (p + 8 < end) {
        Ext4DirEntry2 *de = (Ext4DirEntry2 *)p;
        if (de->rec_len == 0)
            break;
        if (de->inode != 0 &&
            de->name_len == name_len &&
            sb_strncmp8(de->name, name, name_len) == 0) {
            result = de->inode;
            break;
        }
        p += de->rec_len;
    }

    FreePool(dir_data);
    return result;
}

/* ------------------------------------------------------------------ */
/*  Path resolution: /boot/vmlinuz → inode number                      */
/* ------------------------------------------------------------------ */

static UINT32
ext4_resolve_path(Ext4Context *c, const CHAR16 *path)
{
    /* Convert wide path to ASCII. */
    CHAR8 apath[SB_MAX_PATH];
    sb_str16to8(apath, path, sizeof(apath));

    /* Convert backslashes to forward slashes. */
    for (CHAR8 *p = apath; *p; p++)
        if (*p == '\\') *p = '/';

    UINT32 ino = EXT4_ROOT_INO;
    CHAR8 *p = apath;
    if (*p == '/') p++;

    while (*p) {
        /* Extract next component. */
        CHAR8 component[256];
        UINTN ci = 0;
        while (*p && *p != '/' && ci + 1 < sizeof(component))
            component[ci++] = *p++;
        component[ci] = '\0';
        if (*p == '/') p++;

        if (ci == 0) continue;

        Ext4Inode dir;
        if (EFI_ERROR(ext4_read_inode(c, ino, &dir)))
            return 0;

        ino = ext4_dir_lookup(c, &dir, component);
        if (ino == 0)
            return 0;
    }

    return ino;
}

/* ------------------------------------------------------------------ */
/*  VFS driver callbacks                                               */
/* ------------------------------------------------------------------ */

static EFI_STATUS
ext4_probe(EFI_BLOCK_IO_PROTOCOL *block_io, EFI_DISK_IO_PROTOCOL *disk_io)
{
    Ext4Superblock sb;
    EFI_STATUS status;

    if (disk_io) {
        status = disk_io->ReadDisk(
            disk_io, block_io->Media->MediaId,
            EXT4_SUPERBLOCK_OFFSET, sizeof(sb), &sb);
    } else {
        /* Read via Block I/O — need a full sector. */
        UINT32 bs = block_io->Media->BlockSize;
        void *tmp = AllocatePool(bs < 2048 ? 2048 : bs);
        if (!tmp)
            return EFI_OUT_OF_RESOURCES;
        status = block_io->ReadBlocks(
            block_io, block_io->Media->MediaId,
            EXT4_SUPERBLOCK_OFFSET / bs, bs < 2048 ? 2048 : bs, tmp);
        if (!EFI_ERROR(status))
            CopyMem(&sb, (UINT8 *)tmp + (EXT4_SUPERBLOCK_OFFSET % bs),
                    sizeof(sb));
        FreePool(tmp);
    }

    if (EFI_ERROR(status))
        return status;

    return (sb.s_magic == EXT4_SUPER_MAGIC) ? EFI_SUCCESS : EFI_NOT_FOUND;
}

static EFI_STATUS
ext4_mount(EFI_BLOCK_IO_PROTOCOL *block_io, EFI_DISK_IO_PROTOCOL *disk_io,
           void **fs_context)
{
    Ext4Context *c = AllocateZeroPool(sizeof(Ext4Context));
    if (!c)
        return EFI_OUT_OF_RESOURCES;

    c->block_io = block_io;
    c->disk_io  = disk_io;

    /* Read superblock. */
    EFI_STATUS s = ext4_probe(block_io, disk_io);
    if (EFI_ERROR(s)) {
        FreePool(c);
        return s;
    }

    if (disk_io) {
        disk_io->ReadDisk(disk_io, block_io->Media->MediaId,
                          EXT4_SUPERBLOCK_OFFSET, sizeof(c->sb), &c->sb);
    }

    c->block_size = 1024U << c->sb.s_log_block_size;
    c->inode_size = (c->sb.s_rev_level >= 1) ? c->sb.s_inode_size : 128;
    c->group_desc_size = 32; /* 64 if 64-bit feature set. */

    *fs_context = c;
    return EFI_SUCCESS;
}

static EFI_STATUS
ext4_read_file(void *fs_context, const CHAR16 *path,
               void **buffer, UINTN *size)
{
    Ext4Context *c = (Ext4Context *)fs_context;

    UINT32 ino = ext4_resolve_path(c, path);
    if (ino == 0)
        return EFI_NOT_FOUND;

    Ext4Inode inode;
    EFI_STATUS s = ext4_read_inode(c, ino, &inode);
    if (EFI_ERROR(s))
        return s;

    UINT64 file_size = ((UINT64)inode.i_size_high << 32) | inode.i_size_lo;
    *size = (UINTN)file_size;
    *buffer = AllocatePool(*size + 1);
    if (!*buffer)
        return EFI_OUT_OF_RESOURCES;

    s = ext4_read_file_data(c, &inode, *buffer, file_size);
    if (EFI_ERROR(s)) {
        FreePool(*buffer);
        *buffer = NULL;
        return s;
    }
    ((UINT8 *)*buffer)[*size] = 0;

    return EFI_SUCCESS;
}

static EFI_STATUS
ext4_dir_exists(void *fs_context, const CHAR16 *path)
{
    Ext4Context *c = (Ext4Context *)fs_context;
    UINT32 ino = ext4_resolve_path(c, path);
    return (ino != 0) ? EFI_SUCCESS : EFI_NOT_FOUND;
}

static void
ext4_unmount(void *fs_context)
{
    if (fs_context)
        FreePool(fs_context);
}

/* ------------------------------------------------------------------ */
/*  Exported VFS driver                                                */
/* ------------------------------------------------------------------ */

VfsDriver sb_vfs_ext4 = {
    .name       = L"ext4",
    .probe      = ext4_probe,
    .mount      = ext4_mount,
    .read_file  = ext4_read_file,
    .dir_exists = ext4_dir_exists,
    .unmount    = ext4_unmount,
};
