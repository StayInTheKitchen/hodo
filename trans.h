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

#ifndef __TRANS_H__
#define __TRANS_H__

/*-------------------------------------------------------------lookup용 함수 선언-------------------------------------------------------------------------------*/
uint64_t find_inode_number(struct hodo_inode *dir_hodo_inode, const char *target_name);
uint64_t find_inode_number_from_direct_block(struct hodo_datablock *direct_block, const char *target_name);
uint64_t find_inode_number_from_indirect_block(struct hodo_datablock *indirect_block, const char *target_name);

/*-------------------------------------------------------------readdir용 함수 선언-------------------------------------------------------------------------------*/
int read_all_dirents(struct hodo_inode *dir_hodo_inode, struct dir_context *ctx, uint64_t *dirent_count);
int read_all_dirents_from_direct_block(struct hodo_datablock* direct_block,struct dir_context *ctx, uint64_t *dirent_count);
int read_all_dirents_from_indirect_block(struct hodo_datablock* indirect_block, struct dir_context *ctx, uint64_t *dirent_count);

/*-------------------------------------------------------------create용 함수 선언--------------------------------------------------------------------------------*/
int add_dirent(struct inode* dir, struct hodo_inode* sub_inode);

/*-------------------------------------------------------------unlink용 함수 선언--------------------------------------------------------------------------------*/
int remove_dirent(struct hodo_inode *dir_hodo_inode, struct inode *dir, const char *target_name, struct hodo_block_pos *out_pos);
int remove_dirent_from_direct_block(struct hodo_datablock *direct_block, const char *target_name, struct hodo_block_pos *out_pos);
int remove_dirent_from_indirect_block(struct hodo_datablock *indirect_block, const char *target_name, struct hodo_block_pos *out_pos);

/*-------------------------------------------------------------rmdir용 함수 선언--------------------------------------------------------------------------------*/
bool check_directory_empty(struct dentry *dentry);
bool check_directory_empty_from_direct_block(struct hodo_datablock *direct_block);
bool check_directory_empty_from_indirect_block(struct hodo_datablock *indirect_block);

/*-------------------------------------------------------------비트맵용 함수 선언---------------------------------------------------------------------------------*/
int hodo_get_next_ino(void);
int hodo_erase_table_entry(int table_entry_index);

/*-------------------------------------------------------------입출력 함수 선언-----------------------------------------------------------------------------------*/
ssize_t hodo_read_struct(struct hodo_block_pos block_pos, void *out_buf, size_t len);
ssize_t hodo_write_struct(void *buf, size_t len, struct hodo_block_pos *out_pos);
ssize_t compact_datablock(struct hodo_datablock *source_block, int remove_start_index, int remove_size, struct hodo_block_pos *out_pos);
ssize_t hodo_read_on_disk_mapping_info(void);

/*-------------------------------------------------------------도구 함수 선언-------------------------------------------------------------------------------------*/
bool is_dirent_valid(struct hodo_dirent *dirent);
bool is_block_pos_valid(struct hodo_block_pos block_pos);
bool is_directblock(struct hodo_datablock *datablock);

#endif