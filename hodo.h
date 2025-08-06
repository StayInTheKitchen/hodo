/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Simple zone file system for zoned block devices.
 *
 * Copyright (C) 2019 Western Digital Corporation or its affiliates.
 *
 * Added POSIX features to original zonefs.
 *
 * Copyright (C) 2025 StayInTheKitchen, Antler9000
 */

#ifndef __HODO_H__
#define __HODO_H__

#define HODO_MAX_NAME_LEN       16
#define HODO_MAX_INODE          (1 << 16)
#define HODO_DATABLOCK_SIZE     4096
#define HODO_SECTOR_SIZE        512
#define HODO_DATA_START         4

#define HODO_TYPE_REG           0            
#define HODO_TYPE_DIR           1

#define END_READ                0
#define NOTHING_FOUND           0
#define EMPTY_CHECKED           1

#define ZONEFS_TRACE() pr_info("zonefs: >>> %s called\n", __func__)

struct hodo_block_pos {
    uint32_t zone_id;
    uint64_t offset;
};

struct hodo_datablock {
    char magic[4];
    char data[4092];
};

struct hodo_dirent {
    char name[HODO_MAX_NAME_LEN];
    uint8_t name_len;
    uint64_t i_ino;
    uint8_t file_type;
};

struct hodo_inode {
    char magic[4];
    uint64_t file_len;

    uint8_t  name_len;
    char     name[HODO_MAX_NAME_LEN];
    uint8_t  type;

    uint64_t i_ino;
    umode_t  i_mode;
    kuid_t   i_uid;
    kgid_t   i_gid;
    unsigned int i_nlink;

    struct timespec64 i_atime;
    struct timespec64 i_mtime;
    struct timespec64 i_ctime;

    struct hodo_block_pos direct[10];
    struct hodo_block_pos single_indirect;
    struct hodo_block_pos double_indirect;
    struct hodo_block_pos triple_indirect;

    char padding[192];
};

struct hodo_mapping_info {
    struct hodo_block_pos mapping_table[HODO_MAX_INODE];
    int starting_ino;
    struct hodo_block_pos wp;
    uint32_t bitmap[HODO_MAX_INODE / 32];
};

extern char mount_point_path[16];
extern struct hodo_mapping_info mapping_info;

void hodo_init(void);
#endif