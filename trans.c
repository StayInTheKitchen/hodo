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

#include <linux/blkdev.h>
#include <linux/namei.h>

#include "zonefs.h"
#include "hodo.h"
#include "trans.h"

int zone_1_valid_count = 0;
int zone_2_valid_count = 0;

int zone_1_valid_set_count = 0;
int zone_2_valid_set_count = 0;

int zone_1_valid_unset_count = 0;
int zone_2_valid_unset_count = 0;


/*-------------------------------------------------------------static 함수 선언-------------------------------------------------------------------------------*/
static bool hodo_dir_emit(struct dir_context *ctx, struct hodo_dirent *temp_dirent);

static void hodo_set_logical_bitmap(int i, int j);
static void hodo_unset_logical_bitmap(int i, int j);

static void hodo_set_GC_bitmap(struct hodo_block_pos);
static void hodo_unset_GC_bitmap(struct hodo_block_pos);
static struct hodo_block_pos hodo_get_next_GC_valid(void);

static ssize_t hodo_GC_write_struct(void *buf, size_t len, logical_block_number_t *logical_block_number);
static ssize_t hodo_GC_read_struct(struct hodo_block_pos block_pos, void *out_buf, size_t len);
/*----------------------------------------------------------------GC용 함수--------------------------------------------------------------------------------*/
int GC_timing(void) {
    return 1;
}

int GC(void) {
    pr_err("GC : in the GC....\n");
    struct hodo_block_pos prev_wp = mapping_info.wp;

    mapping_info.wp.zone_id = 1;
    mapping_info.wp.block_index = 0;

    mapping_info.swap_wp.zone_id = hodo_nr_zones - 2;
    mapping_info.swap_wp.block_index = 0;

    struct hodo_block_pos valid_block_pos = {0,0};
    struct hodo_datablock* temp_datablock = kmalloc(HODO_DATABLOCK_SIZE, GFP_KERNEL);

    valid_block_pos = hodo_get_next_GC_valid();
    while (valid_block_pos.zone_id != 0) {
        hodo_GC_read_struct(valid_block_pos, temp_datablock, HODO_DATABLOCK_SIZE);

        logical_block_number_t logical_block_number;
        if (((char*)temp_datablock)[0] == 'D' && ((char*)temp_datablock)[1] == 'A' && ((char*)temp_datablock)[2] == 'T') {
            logical_block_number = ((struct hodo_datablock*)temp_datablock)->logical_block_number;
        }
        else {
            logical_block_number = ((struct hodo_inode*)temp_datablock)->i_ino;
        }

        hodo_GC_write_struct(temp_datablock, HODO_DATABLOCK_SIZE, &logical_block_number);

        if (mapping_info.swap_wp.block_index >= BLOCKS_PER_ZONE-1) {
            struct hodo_block_pos swap_out_ptr = {hodo_nr_zones-2, 0};

            // wp zone을 reset
            struct path path;
            struct inode *inode;
            char path_buf[32];

            scnprintf(path_buf, sizeof(path_buf), "%s/seq/%d", mount_point_path, mapping_info.wp.zone_id);

            if (kern_path(path_buf, LOOKUP_FOLLOW, &path) == 0) {
                inode = d_inode(path.dentry);
                zonefs_file_truncate(inode, 0);
                path_put(&path);
            } else {
                pr_err("Failed to resolve path: %s\n", path_buf);
            }

            while (1) {
                pr_info("swapout_block index, BLOCK_PER_ZONE (%d,%d)\n", swap_out_ptr.block_index, BLOCKS_PER_ZONE);
                hodo_GC_read_struct(swap_out_ptr, temp_datablock, HODO_DATABLOCK_SIZE);

                logical_block_number_t swap_logical_block_number;
                if (((char*)temp_datablock)[0] == 'D' && ((char*)temp_datablock)[1] == 'A' && ((char*)temp_datablock)[2] == 'T') {
                    swap_logical_block_number = ((struct hodo_datablock*)temp_datablock)->logical_block_number;
                }
                else {
                    swap_logical_block_number = ((struct hodo_inode*)temp_datablock)->i_ino;
                }

                hodo_write_struct(temp_datablock, HODO_DATABLOCK_SIZE, &swap_logical_block_number);
                pr_info("mapping info wp: (%d, %d)\n", mapping_info.wp.zone_id, mapping_info.wp.block_index);

                if (swap_out_ptr.block_index == BLOCKS_PER_ZONE-1) {
                    break;
                }
                swap_out_ptr.block_index++;
            }
            pr_info("not infiinite\n");

            // swap_wp zone을 reset
            scnprintf(path_buf, sizeof(path_buf), "%s/seq/%d", mount_point_path, mapping_info.swap_wp.zone_id);

            if (kern_path(path_buf, LOOKUP_FOLLOW, &path) == 0) {
                inode = d_inode(path.dentry);
                zonefs_file_truncate(inode, 0);
                path_put(&path);
            } else {
                pr_err("Failed to resolve path: %s\n", path_buf);
            }

            mapping_info.swap_wp.block_index = 0;
        }

        valid_block_pos = hodo_get_next_GC_valid();
    }

    // 꼬투리 zone
    if (mapping_info.swap_wp.block_index != 0) {
        // pr_info("KOTORI called!\n");
        struct hodo_block_pos swap_out_ptr = {hodo_nr_zones-2, 0};

        // wp zone을 reset
        struct path path;
        struct inode *inode;
        char path_buf[32];

        scnprintf(path_buf, sizeof(path_buf), "%s/seq/%d", mount_point_path, mapping_info.wp.zone_id);

        if (kern_path(path_buf, LOOKUP_FOLLOW, &path) == 0) {
            inode = d_inode(path.dentry);
            zonefs_file_truncate(inode, 0);
            path_put(&path);
        } else {
            pr_err("Failed to resolve path: %s\n", path_buf);
        }

        while (swap_out_ptr.block_index < mapping_info.swap_wp.block_index) {
            hodo_GC_read_struct(swap_out_ptr, temp_datablock, HODO_DATABLOCK_SIZE);

            logical_block_number_t swap_logical_block_number;
            if (((char*)temp_datablock)[0] == 'D' && ((char*)temp_datablock)[1] == 'A' && ((char*)temp_datablock)[2] == 'T') {
                swap_logical_block_number = ((struct hodo_datablock*)temp_datablock)->logical_block_number;
            }
            else {
                swap_logical_block_number = ((struct hodo_inode*)temp_datablock)->i_ino;
            }

            hodo_write_struct(temp_datablock, HODO_DATABLOCK_SIZE, &swap_logical_block_number);
            swap_out_ptr.block_index++;
        }

        // swap_wp zone을 reset
        scnprintf(path_buf, sizeof(path_buf), "%s/seq/%d", mount_point_path, mapping_info.swap_wp.zone_id);

        if (kern_path(path_buf, LOOKUP_FOLLOW, &path) == 0) {
            inode = d_inode(path.dentry);
            zonefs_file_truncate(inode, 0);
            path_put(&path);
        } else {
            pr_err("Failed to resolve path: %s\n", path_buf);
        }
    }

    int start = mapping_info.wp.zone_id + 1; 
    if (mapping_info.wp.block_index == 0) {
        start--;
    }

    for (int i = start; i <= prev_wp.zone_id; ++i) {
        // wp zone을 reset
        struct path path;
        struct inode *inode;
        char path_buf[32];

        scnprintf(path_buf, sizeof(path_buf), "%s/seq/%d", mount_point_path, i);

        if (kern_path(path_buf, LOOKUP_FOLLOW, &path) == 0) {
            inode = d_inode(path.dentry);
            zonefs_file_truncate(inode, 0);
            path_put(&path);
        } else {
            pr_err("Failed to resolve path: %s\n", path_buf);
        }
    }


    kfree(temp_datablock);

    return 0;
}

/*-----------------------------------------------------------read_iter용 함수------------------------------------------------------------------------------*/
// file_inode에서 n번째 datablock을 dst_datablock으로 copy
void hodo_read_nth_block(struct hodo_inode *file_inode, int n, struct hodo_datablock *dst_datablock) {
    // ZONEFS_TRACE();
    int num_direct_block = sizeof(file_inode->direct) / sizeof((file_inode->direct)[0]);  // direct block의 개수
    int num_block_pos_in_indirect_block = HODO_DATA_SIZE / BLOCK_PTR_SZ;

    if (n < num_direct_block) { // direct block
        // pr_info("read direct block\n");
        logical_block_number_t data_block_logical_number = file_inode->direct[n];
        hodo_read_struct(data_block_logical_number, dst_datablock, sizeof(struct hodo_datablock));
    }
    else if (n < (num_direct_block + num_block_pos_in_indirect_block)) {    // single indirect block
        // pr_info("read single indirect block\n");
        int nth_in_direct_block = n - num_direct_block;

        struct hodo_datablock* temp_datablock = kmalloc(HODO_DATABLOCK_SIZE, GFP_KERNEL);
        logical_block_number_t direct_block_logical_number = file_inode->single_indirect;
        hodo_read_struct(direct_block_logical_number, temp_datablock, sizeof(struct hodo_datablock));  // read direct block

        logical_block_number_t data_block_logical_number;
        memcpy(&data_block_logical_number, (char*)temp_datablock + HODO_DATA_START + (nth_in_direct_block * BLOCK_PTR_SZ), BLOCK_PTR_SZ);
        hodo_read_struct(data_block_logical_number, dst_datablock, sizeof(struct hodo_datablock));

        kfree(temp_datablock);
    }
    else if (n < (num_direct_block + num_block_pos_in_indirect_block + (num_block_pos_in_indirect_block * num_block_pos_in_indirect_block))) {    // double indirect block
        // pr_info("read double indirect block\n");
        int nth_in_single_indirect_block = (n - (num_direct_block + num_block_pos_in_indirect_block)) / num_block_pos_in_indirect_block;
        int nth_in_direct_block = (n - (num_direct_block + num_block_pos_in_indirect_block)) % num_block_pos_in_indirect_block;

        struct hodo_datablock *temp_datablock = kmalloc(HODO_DATABLOCK_SIZE, GFP_KERNEL);
        logical_block_number_t single_indirect_logical_number = file_inode->double_indirect;
        hodo_read_struct(single_indirect_logical_number, temp_datablock, sizeof(struct hodo_datablock));

        logical_block_number_t direct_block_logical_number = 0;
        memcpy(&direct_block_logical_number, (char*)temp_datablock + HODO_DATA_START + (nth_in_single_indirect_block * BLOCK_PTR_SZ), BLOCK_PTR_SZ);
        hodo_read_struct(direct_block_logical_number, temp_datablock, sizeof(struct hodo_datablock));

        logical_block_number_t data_block_logical_number= 0;
        memcpy(&data_block_logical_number, (char*)temp_datablock + HODO_DATA_START + (nth_in_direct_block * BLOCK_PTR_SZ), BLOCK_PTR_SZ);
        hodo_read_struct(data_block_logical_number, dst_datablock, sizeof(struct hodo_datablock));

        kfree(temp_datablock);
    }
    else {  // triple indirect block
        // pr_info("read triple indirect block\n");
        int nth_in_double_indirect_block = (n - (num_direct_block + num_block_pos_in_indirect_block + (num_block_pos_in_indirect_block * num_block_pos_in_indirect_block)));
        nth_in_double_indirect_block /= (num_block_pos_in_indirect_block * num_block_pos_in_indirect_block);

        int nth_in_single_indirect_block = (n - (num_direct_block + num_block_pos_in_indirect_block + (num_block_pos_in_indirect_block * num_block_pos_in_indirect_block)));
        nth_in_single_indirect_block %= (num_block_pos_in_indirect_block * num_block_pos_in_indirect_block);
        nth_in_single_indirect_block /= num_block_pos_in_indirect_block;

        int nth_in_direct_block = (n - (num_direct_block + num_block_pos_in_indirect_block + (num_block_pos_in_indirect_block * num_block_pos_in_indirect_block)));
        nth_in_direct_block %= (num_block_pos_in_indirect_block * num_block_pos_in_indirect_block); 
        nth_in_direct_block %= num_block_pos_in_indirect_block;

        struct hodo_datablock *temp_datablock = kmalloc(HODO_DATABLOCK_SIZE, GFP_KERNEL);

        logical_block_number_t double_indirect_block_logical_number = file_inode->triple_indirect;
        hodo_read_struct(double_indirect_block_logical_number, temp_datablock, sizeof(struct hodo_datablock));

        logical_block_number_t single_indirect_block_logical_number = 0;
        memcpy(&single_indirect_block_logical_number, (char*)temp_datablock + HODO_DATA_START + (nth_in_double_indirect_block * BLOCK_PTR_SZ), BLOCK_PTR_SZ);
        hodo_read_struct(single_indirect_block_logical_number, temp_datablock, sizeof(struct hodo_datablock));

        logical_block_number_t direct_block_logical_number = 0;
        memcpy(&direct_block_logical_number, (char*)temp_datablock + HODO_DATA_START + (nth_in_single_indirect_block * BLOCK_PTR_SZ), BLOCK_PTR_SZ);
        hodo_read_struct(direct_block_logical_number, temp_datablock, sizeof(struct hodo_datablock));

        logical_block_number_t data_block_logical_number = 0;
        memcpy(&data_block_logical_number, (char*)temp_datablock + HODO_DATA_START + (nth_in_direct_block * BLOCK_PTR_SZ), BLOCK_PTR_SZ);
        hodo_read_struct(data_block_logical_number, dst_datablock, sizeof(struct hodo_datablock));

        kfree(temp_datablock);
    }
    return;
}

/*-------------------------------------------------------------write_iter용 함수 선언----------------------------------------------------------------------------*/
ssize_t write_one_block(struct kiocb *iocb, struct iov_iter *from){
    // ZONEFS_TRACE();

    //write iter의 대상은 a.txt와 같은 일반 파일이므로, 해당 아이노드는 절대로 루트 디렉토리 아이노드일 수가 없다.
    //따라서 굳이 타겟 아이노드가 루트 아이노드인지를 확인해서 매핑 인덱스 0번을 수동으로 할당할 필요가 없다.
    struct inode *target_inode = iocb->ki_filp->f_inode;
    uint64_t target_ino = target_inode->i_ino;
    uint64_t target_mapping_index = target_ino;

    //타겟 파일의 아이노드를 불러오기.
    struct hodo_inode target_hodo_inode;
    logical_block_number_t target_inode_logical_number;

    target_inode_logical_number = target_mapping_index;
    hodo_read_struct(target_inode_logical_number, &target_hodo_inode, sizeof(struct hodo_inode));

    if (iocb->ki_flags & IOCB_APPEND)
        iocb->ki_pos = i_size_read(target_inode);
    // pr_info("zonefs: write_iter original target offset is %d, new i_size is %d\n", iocb->ki_pos, target_inode->i_size);
    
    //어디에다가 파일을 쓸지를 추가한다.
    struct hodo_datablock *target_block = kmalloc(HODO_DATABLOCK_SIZE, GFP_KERNEL);
    if (target_block == NULL) {
        // pr_info("zonefs: (error in hodo_sub_file_write_iter) cannot allocate 4KB heap space for datablock variable\n");
        return -ENOMEM;
    }

    uint64_t num_of_block_poses_in_datablock = HODO_DATA_SIZE/BLOCK_PTR_SZ;

    loff_t offset = iocb->ki_pos;
    loff_t data_block_index = offset / HODO_DATA_SIZE;
    uint64_t num_of_direct_blocks_in_hodo_inode            = 10;
    uint64_t num_of_direct_blocks_in_single_indirect_block = num_of_block_poses_in_datablock;
    uint64_t num_of_direct_blocks_in_double_indirect_block = num_of_block_poses_in_datablock * num_of_block_poses_in_datablock;
    uint64_t num_of_direct_blocks_in_triple_indirect_block = num_of_block_poses_in_datablock * num_of_block_poses_in_datablock * num_of_block_poses_in_datablock;

    ssize_t written_size = 0;
    if(data_block_index         < num_of_direct_blocks_in_hodo_inode){
        //direct_data_block(0~9)에 쓸 위치가 존재
        // pr_info("zonefs: write_iter in direct_data_block\n");

        if(is_block_logical_number_valid(target_hodo_inode.direct[data_block_index]))
            hodo_read_struct(target_hodo_inode.direct[data_block_index], target_block, HODO_DATABLOCK_SIZE);
        else {
            target_block->magic[0] = 'D';
            target_block->magic[1] = 'A';
            target_block->magic[2] = 'T';
            target_block->magic[3] = '0';
        }

        logical_block_number_t written_logical_number = target_hodo_inode.direct[data_block_index];
        written_size = write_one_block_by_direct_block(iocb, from, &written_logical_number, target_block);

        target_hodo_inode.direct[data_block_index] = written_logical_number;
        target_hodo_inode.file_len = iocb->ki_pos + written_size;
    } 
    else if(data_block_index    < num_of_direct_blocks_in_hodo_inode + num_of_direct_blocks_in_single_indirect_block){
        //singe_indirect_data_block에 쓸 위치가 존재
        // pr_info("zonefs: write_iter in single_indirect_data_block\n");

        if(is_block_logical_number_valid(target_hodo_inode.single_indirect))
            hodo_read_struct(target_hodo_inode.single_indirect, target_block, HODO_DATABLOCK_SIZE);
        else {
            target_block->magic[0] = 'D';
            target_block->magic[1] = 'A';
            target_block->magic[2] = 'T';
            target_block->magic[3] = '1';
        }

        logical_block_number_t written_logical_number = target_hodo_inode.single_indirect;
        written_size = write_one_block_by_indirect_block(iocb, from, &written_logical_number, target_block);

        target_hodo_inode.single_indirect = written_logical_number;
        target_hodo_inode.file_len = iocb->ki_pos + written_size;
    }
    else if(data_block_index    < num_of_direct_blocks_in_hodo_inode + num_of_direct_blocks_in_single_indirect_block + num_of_direct_blocks_in_double_indirect_block){
        //double_indirect_data_block에 쓸 위치가 존재
        // pr_info("zonefs: write_iter in double_indirect_data_block\n");

        if(is_block_logical_number_valid(target_hodo_inode.double_indirect))
            hodo_read_struct(target_hodo_inode.double_indirect, target_block, HODO_DATABLOCK_SIZE);
        else {
            target_block->magic[0] = 'D';
            target_block->magic[1] = 'A';
            target_block->magic[2] = 'T';
            target_block->magic[3] = '2';
        }

        logical_block_number_t written_logical_number = target_hodo_inode.double_indirect;
        written_size = write_one_block_by_indirect_block(iocb, from, &written_logical_number, target_block);

        target_hodo_inode.double_indirect = written_logical_number;
        target_hodo_inode.file_len = iocb->ki_pos + written_size;
    }
    else if(data_block_index    < num_of_direct_blocks_in_hodo_inode + num_of_direct_blocks_in_single_indirect_block + num_of_direct_blocks_in_double_indirect_block + num_of_direct_blocks_in_triple_indirect_block){
        //triple_indirect_data_block에 쓸 위치가 존재
        // pr_info("zonefs: write_iter in triple_indirect_data_block\n");

        if(is_block_logical_number_valid(target_hodo_inode.triple_indirect))
            hodo_read_struct(target_hodo_inode.triple_indirect, target_block, HODO_DATABLOCK_SIZE);
        else {
            target_block->magic[0] = 'D';
            target_block->magic[1] = 'A';
            target_block->magic[2] = 'T';
            target_block->magic[3] = '3';
        }

        logical_block_number_t written_logical_number = target_hodo_inode.triple_indirect;
        written_size = write_one_block_by_indirect_block(iocb, from, &written_logical_number, target_block);

        target_hodo_inode.triple_indirect = written_logical_number;
        target_hodo_inode.file_len = iocb->ki_pos + written_size;
    }
    else {
        //파일시스템 상 파일의 최대 크기를 넘어선 오프셋에는 쓰기가 불가능 하다
        // pr_info("zonefs: (error in hodo_sub_file_write_iter) write_iter over the triple_indirect_data_block. we cannot do it.\n");
        kfree(target_block);
        return -EFBIG;
    }

    //데이터 블록이 새로 써졌으므로, 파일의 hodo 아이노드도 새로 쓰도록 한다
    hodo_write_struct(&target_hodo_inode, sizeof(struct hodo_inode), &target_inode_logical_number);

    //실제로 쓰기가 수행된 길이를 반환한다. 만약 이것이 요청된 쓰기 길이에 미치지 못한다면, VFS는 나머지 부분을 재호출 할 것이다.
    kfree(target_block);

    iocb->ki_pos += written_size;
    i_size_write(target_inode, iocb->ki_pos);
    // pr_info("zonefs: write_iter new target offset is %d, new i_size is %d\n", iocb->ki_pos, target_inode->i_size);
    return written_size;
}

ssize_t write_one_block_by_direct_block(struct kiocb *iocb, struct iov_iter *from, logical_block_number_t *out_logical_number, struct hodo_datablock *current_direct_block){
    // ZONEFS_TRACE();

    struct hodo_datablock *target_block = kmalloc(HODO_DATABLOCK_SIZE, GFP_KERNEL);
    if (target_block == NULL) {
        // pr_info("zonefs: (error in hodo_sub_file_write_iter) cannot allocate 4KB heap space for datablock variable\n");
        return -ENOMEM;
    }

    target_block->magic[0] = 'D';
    target_block->magic[1] = 'A';
    target_block->magic[2] = 'T';
    target_block->magic[3] = '0';

    loff_t offset = iocb->ki_pos;
    uint64_t len = iov_iter_count(from);

    //쓰고자 하는 데이터를 실제로 데이터 블록에다가 쓴다. 단, 우리가 이 함수 한 번에서 쓰는 양은 한 블록을 넘지 않는다.
    //쓰려고 하는 데이터 양이 데이터 블락을 넘어서는 경우는, VFS가 알아서 쓰기 잔여량을 보고서 재호출하는 기능에 의지하도록 한다.
    uint64_t left_len = offset % HODO_DATA_SIZE;
    if((left_len != 0) && current_direct_block != NULL) {
        //current_direct_block의 일부에 이미 이전에 쓰인 데이터(left over)가 있다면, 이들을 새로 쓸 데이터랑 합쳐서 쓰도록 한다.
        // pr_info("zonefs: write_iter with left over\n");
        memcpy(target_block->data, current_direct_block->data, left_len);

        if(len + left_len >= HODO_DATA_SIZE){
            //left over를 합쳐서 쓰려고 하는 데이터가 데이터 블럭을 넘어선다면 하나의 블럭 크기만 쓴다.
            if(copy_from_iter((void *)(target_block->data) + left_len, HODO_DATA_SIZE - left_len, from) != HODO_DATA_SIZE - left_len){
                kfree(target_block);
                return -EFAULT;
            }

            hodo_write_struct(target_block, HODO_DATABLOCK_SIZE, out_logical_number);
            kfree(target_block);
            return HODO_DATA_SIZE - left_len;
        }
        else {
            //left over을 합쳐서 쓰려고 하는 데이터가 데이터 블럭 크기 이내라면 그만큼만 쓴다.
            if(copy_from_iter((void *)(target_block->data) + left_len, len, from) != len){
                kfree(target_block);
                return -EFAULT;
            }

            hodo_write_struct(target_block, HODO_DATABLOCK_SIZE, out_logical_number);
            kfree(target_block);
            return len;
        }
    }
    else {
        //쓰려고 하는 블록이 깔끔하다면, 새로 쓰고자 하는 내용만 쓴다.
        // pr_info("zonefs: write_iter without left over\n");
        if(len >= HODO_DATA_SIZE){
            //쓰려고 하는 데이터가 데이터 블럭을 넘어선다면 하나의 블럭 크기만 쓴다.
            if(copy_from_iter(target_block->data, HODO_DATA_SIZE, from) != HODO_DATA_SIZE){
                kfree(target_block);
                return -EFAULT;
            }

            hodo_write_struct(target_block, HODO_DATABLOCK_SIZE, out_logical_number);
            kfree(target_block);
            return HODO_DATA_SIZE;
        }
        else {
            //쓰려고 하는 데이터가 데이터 블럭 크기 이내라면 그만큼만 쓴다.
            if(copy_from_iter(target_block->data, len, from) != len){
                kfree(target_block);
                return -EFAULT;
            }

            hodo_write_struct(target_block, HODO_DATABLOCK_SIZE, out_logical_number);
            kfree(target_block);
            return len;
        }
    }
}

ssize_t write_one_block_by_indirect_block(struct kiocb *iocb, struct iov_iter *from, logical_block_number_t *out_logical_number, struct hodo_datablock *current_indirect_block){
    // ZONEFS_TRACE();

    //offset을 통해서, 현재 indirect_block 속에 나열 된 block_pos중 무엇을 사용해야 할지 알아낸다.
    uint64_t block_pos_index_in_current_indirect_block = 0;

    uint64_t num_of_block_poses_in_datablock = HODO_DATA_SIZE/BLOCK_PTR_SZ;

    loff_t offset = iocb->ki_pos;
    loff_t data_block_index = offset / HODO_DATA_SIZE;
    uint64_t num_of_direct_blocks_in_hodo_inode            = 10;
    uint64_t num_of_direct_blocks_in_single_indirect_block = num_of_block_poses_in_datablock;
    uint64_t num_of_direct_blocks_in_double_indirect_block = num_of_block_poses_in_datablock * num_of_block_poses_in_datablock;
    uint64_t num_of_direct_blocks_in_triple_indirect_block = num_of_block_poses_in_datablock * num_of_block_poses_in_datablock * num_of_block_poses_in_datablock;

    if(data_block_index         < num_of_direct_blocks_in_hodo_inode){
        // pr_info("zonefs: (error in hodo_sub_file_write_iter) processing in direct block should not reach here\n");
        return -EIO;
    }
    else if(data_block_index    < num_of_direct_blocks_in_hodo_inode + num_of_direct_blocks_in_single_indirect_block){
        uint64_t block_pos_index_in_single_indirect_datablock = (data_block_index - num_of_direct_blocks_in_hodo_inode);
        block_pos_index_in_current_indirect_block = block_pos_index_in_single_indirect_datablock;
        // pr_info("DAT1 : calculated offset in indirect_block : %d\n", block_pos_index_in_current_indirect_block);
    }
    else if(data_block_index    < num_of_direct_blocks_in_hodo_inode + num_of_direct_blocks_in_single_indirect_block + num_of_direct_blocks_in_double_indirect_block){
        uint64_t block_pos_index_in_double_indirect_datablock = (data_block_index - num_of_direct_blocks_in_hodo_inode - num_of_direct_blocks_in_single_indirect_block) / num_of_block_poses_in_datablock;
        uint64_t block_pos_index_in_single_indirect_datablock = (data_block_index - num_of_direct_blocks_in_hodo_inode - num_of_direct_blocks_in_single_indirect_block) % num_of_block_poses_in_datablock;
        if(
            current_indirect_block->magic[0] == 'D' &&
            current_indirect_block->magic[1] == 'A' &&
            current_indirect_block->magic[2] == 'T' &&
            current_indirect_block->magic[3] == '2'
        ){
            block_pos_index_in_current_indirect_block = block_pos_index_in_double_indirect_datablock;
        }
        else if(
            current_indirect_block->magic[0] == 'D' &&
            current_indirect_block->magic[1] == 'A' &&
            current_indirect_block->magic[2] == 'T' &&
            current_indirect_block->magic[3] == '1'
        ){
            block_pos_index_in_current_indirect_block = block_pos_index_in_single_indirect_datablock;
        }
        else {
            return -EIO;
        }

        // pr_info("DAT2 : calculated offset in indirect_block : %d\n", block_pos_index_in_current_indirect_block);
    }
    else if(data_block_index    < num_of_direct_blocks_in_hodo_inode + num_of_direct_blocks_in_single_indirect_block + num_of_direct_blocks_in_double_indirect_block + num_of_direct_blocks_in_triple_indirect_block){
        uint64_t block_pos_index_in_triple_indirect_datablock = (data_block_index - num_of_direct_blocks_in_hodo_inode - num_of_direct_blocks_in_single_indirect_block - num_of_direct_blocks_in_double_indirect_block) / num_of_block_poses_in_datablock / num_of_block_poses_in_datablock;
        uint64_t block_pos_index_in_double_indirect_datablock = (data_block_index - num_of_direct_blocks_in_hodo_inode - num_of_direct_blocks_in_single_indirect_block - num_of_direct_blocks_in_double_indirect_block) / num_of_block_poses_in_datablock % num_of_block_poses_in_datablock;
        uint64_t block_pos_index_in_single_indirect_datablock = (data_block_index - num_of_direct_blocks_in_hodo_inode - num_of_direct_blocks_in_single_indirect_block - num_of_direct_blocks_in_double_indirect_block) % num_of_block_poses_in_datablock;
        if(
            current_indirect_block->magic[0] == 'D' &&
            current_indirect_block->magic[1] == 'A' &&
            current_indirect_block->magic[2] == 'T' &&
            current_indirect_block->magic[3] == '3'
        ){
            block_pos_index_in_current_indirect_block = block_pos_index_in_triple_indirect_datablock;
        }
        else if(
            current_indirect_block->magic[0] == 'D' &&
            current_indirect_block->magic[1] == 'A' &&
            current_indirect_block->magic[2] == 'T' &&
            current_indirect_block->magic[3] == '2'
        ){
            block_pos_index_in_current_indirect_block = block_pos_index_in_double_indirect_datablock;
        }
        else if(
            current_indirect_block->magic[0] == 'D' &&
            current_indirect_block->magic[1] == 'A' &&
            current_indirect_block->magic[2] == 'T' &&
            current_indirect_block->magic[3] == '1'
        ){
            block_pos_index_in_current_indirect_block = block_pos_index_in_single_indirect_datablock;
        }
        else {
            return -EIO;
        }
    }
    else{
        // pr_info("zonefs: (error in hodo_sub_file_write_iter) write_iter over the triple_indirect_data_block. we cannot do it.\n");
        return -EFBIG;
    }

    void* address_of_target_block_pos_in_current_block = (void*)(current_indirect_block->data) + block_pos_index_in_current_indirect_block * BLOCK_PTR_SZ;
    logical_block_number_t target_block_logical_number = 0;
    memcpy(&target_block_logical_number, address_of_target_block_pos_in_current_block, BLOCK_PTR_SZ);

    //위에서 알아낸 block_pos가 유효하다면 해당 데이터블록을 저장장치로부터 읽어낸다. 이렇게 읽어낸 블록은 이전 데이터 블록보다 간접 차수가 1 낮은 데이터 블록이다.
    struct hodo_datablock *target_block = kmalloc(HODO_DATABLOCK_SIZE, GFP_KERNEL);
    if (target_block == NULL) {
        // pr_info("zonefs: (error in hodo_sub_file_write_iter) cannot allocate 4KB heap space for datablock variable\n");
        return -ENOMEM;
    }

    logical_block_number_t written_logical_number = 0;
    //호출할 함수가 write_one_block_by_indirect_block인지, write_one_block_by_direct_block인지를 분간해서 재귀를 호출한다.
    //분간하기 위해서 유효한 target_block이라면 magic을 확인한다.
    if(is_block_logical_number_valid(target_block_logical_number)) {
        hodo_read_struct(target_block_logical_number, target_block, HODO_DATABLOCK_SIZE);

        written_logical_number = target_block_logical_number;
    }
    else {
        target_block->magic[0] = 'D';
        target_block->magic[1] = 'A';
        target_block->magic[2] = 'T';
        target_block->magic[3] = current_indirect_block->magic[3] - 1;
    }

    ssize_t written_size = 0;

    if(is_directblock(target_block)){
        // pr_info("DAT1->DAT0 : calculated offset in indirect_block : %d\n", block_pos_index_in_current_indirect_block);
        written_size = write_one_block_by_direct_block(iocb, from, &written_logical_number, target_block);
    }
    else
        written_size = write_one_block_by_indirect_block(iocb, from, &written_logical_number, target_block);

    //아래 차수의 데이터 블록이 새로 써졌으니, 이를 가리키는 block pos도 새 것으로 대체해서 현재 데이터 블록도 새로 쓰고서 그 위치를 상위 함수에 반환하여야 한다.
    if (written_size > 0 && is_block_logical_number_valid(written_logical_number)){
        memcpy(address_of_target_block_pos_in_current_block, &written_logical_number, BLOCK_PTR_SZ);
        hodo_write_struct(current_indirect_block, sizeof(struct hodo_datablock), out_logical_number);
    }

    kfree(target_block);
    return written_size;
}


/*-------------------------------------------------------------lookup용 함수----------------------------------------------------------------------------------*/
uint64_t find_inode_number(struct hodo_inode *dir_hodo_inode, const char *target_name) {
    // ZONEFS_TRACE();

    uint64_t result;
    struct hodo_datablock *buf_block = kmalloc(HODO_DATABLOCK_SIZE, GFP_KERNEL);

    if (buf_block == NULL) {
        // pr_info("zonefs: (error in hodo_sub_lookup) cannot allocate 4KB heap space for datablock variable\n");
        return NOTHING_FOUND;
    }

    //direct data block들에서 특정 이름의 hodo dentry를 찾아보기
    logical_block_number_t direct_block_logical_number;

    for (int i = 0; i < 10; i++) {
        direct_block_logical_number = dir_hodo_inode->direct[i];
        
        if(is_block_logical_number_valid(direct_block_logical_number)) {
            hodo_read_struct(direct_block_logical_number, buf_block, HODO_DATABLOCK_SIZE);

            result = find_inode_number_from_direct_block(buf_block, target_name);

            if (result != NOTHING_FOUND) {
                kfree(buf_block);
                return result;
            }
        }
    }

    //single, double, triple indirect data block에서 특정 이름의 hodo dentry를 찾아보기
    logical_block_number_t indirect_block_logical_number[3] = {
        dir_hodo_inode->single_indirect,
        dir_hodo_inode->double_indirect,
        dir_hodo_inode->triple_indirect
    };

    for(int i = 0; i < 3; i++){
        if(is_block_logical_number_valid(indirect_block_logical_number[i])) {
            hodo_read_struct(indirect_block_logical_number[i], buf_block, HODO_DATABLOCK_SIZE);

            result = find_inode_number_from_indirect_block(buf_block, target_name);

            if (result != NOTHING_FOUND) {
                kfree(buf_block);
                return result;
            }
        }
    }

    //모두 다 뒤져보았지만 찾는데 실패한 경우
    kfree(buf_block);
    return NOTHING_FOUND;
}

uint64_t find_inode_number_from_direct_block(
    struct hodo_datablock* direct_block,
    const char *target_name
) {
    // ZONEFS_TRACE();

    for (int j = HODO_DATA_START; j < HODO_DATABLOCK_SIZE - sizeof(struct hodo_dirent); j += sizeof(struct hodo_dirent)) {
        struct hodo_dirent temp_dirent;
        memcpy(&temp_dirent, (void*)direct_block + j, sizeof(struct hodo_dirent));

        if (memcmp(temp_dirent.name, target_name, HODO_MAX_NAME_LEN) == 0)
            return temp_dirent.i_ino;
    }

    return NOTHING_FOUND;
}

uint64_t find_inode_number_from_indirect_block(
    struct hodo_datablock *indirect_block,
    const char *target_name
) {
    // ZONEFS_TRACE();

    logical_block_number_t temp_block_logical_number;
    struct hodo_datablock *temp_block = kmalloc(HODO_DATABLOCK_SIZE, GFP_KERNEL);

    if (temp_block == NULL) {
        // pr_info("zonefs: (error in hodo_sub_lookup) cannot allocate 4KB heap space for datablock variable\n");
        return NOTHING_FOUND;
    }

    for (int j = HODO_DATA_START; j < HODO_DATABLOCK_SIZE - BLOCK_PTR_SZ; j += BLOCK_PTR_SZ) {
        memcpy(&temp_block_logical_number, (void*)indirect_block + j, BLOCK_PTR_SZ);

        if(!is_block_logical_number_valid(temp_block_logical_number))
            continue;

        hodo_read_struct(temp_block_logical_number, temp_block, HODO_DATABLOCK_SIZE);

        uint64_t result;

        if(is_directblock(temp_block))
            result = find_inode_number_from_direct_block(temp_block, target_name);

        else 
            result = find_inode_number_from_indirect_block(temp_block, target_name);

        if (result != NOTHING_FOUND){
            kfree(temp_block);
            return result;
        }
    }

    kfree(temp_block);
    return NOTHING_FOUND;
}

/*-------------------------------------------------------------readdir용 함수-------------------------------------------------------------------------------*/
int read_all_dirents(
    struct hodo_inode *dir_hodo_inode, 
    struct dir_context *ctx, 
    uint64_t *dirent_count
) {
    // ZONEFS_TRACE();

    struct hodo_datablock *buf_block = kmalloc(HODO_DATABLOCK_SIZE, GFP_KERNEL);

    if (buf_block == NULL) {
        // pr_info("zonefs: (error in hodo_sub_readdir) cannot allocate 4KB heap space for datablock variable\n");
        return END_READ;
    }

    int result;

    //direct data block들에서 hodo dirent들을 읽어내기
    logical_block_number_t direct_block_logical_number;

    for (int i = 0; i < 10; i++) {
        direct_block_logical_number = dir_hodo_inode->direct[i];
        
        if(is_block_logical_number_valid(direct_block_logical_number)) {
            hodo_read_struct(direct_block_logical_number, buf_block, HODO_DATABLOCK_SIZE);

            result = read_all_dirents_from_direct_block(buf_block, ctx, dirent_count);
            
            if(result == END_READ) {
                kfree(buf_block);
                return result;
            }
        }
    }

    //single, double, triple indirect data block에서 hodo dirent들을 읽어내기
    logical_block_number_t indirect_block_logical_number[3] = {
        dir_hodo_inode->single_indirect,
        dir_hodo_inode->double_indirect,
        dir_hodo_inode->triple_indirect
    };

    for(int i = 0; i < 3; i++){
        if(is_block_logical_number_valid(indirect_block_logical_number[i])) {
            hodo_read_struct(indirect_block_logical_number[i], buf_block, HODO_DATABLOCK_SIZE);

            result = read_all_dirents_from_indirect_block(buf_block, ctx, dirent_count);

            if(result == END_READ) {
                kfree(buf_block);
                return result;
            }
        }
    }

    //모두 잘 다 뒤져보았으므로 더 읽을 필요가 없음을 알려주기 위해 END_READ를 반환하고 끝낸다
    kfree(buf_block);
    return END_READ;
}

int read_all_dirents_from_direct_block(
    struct hodo_datablock* direct_block,
    struct dir_context *ctx,
    uint64_t *dirent_count
) {
    // ZONEFS_TRACE();

    for (int j = HODO_DATA_START; j < HODO_DATABLOCK_SIZE - sizeof(struct hodo_dirent); j += sizeof(struct hodo_dirent)) {
        struct hodo_dirent temp_dirent;
        memcpy(&temp_dirent, (void*)direct_block + j, sizeof(struct hodo_dirent));
        
        if(is_dirent_valid(&temp_dirent)) {
            if(*dirent_count == ctx->pos) {
                hodo_dir_emit(ctx, &temp_dirent);
                ctx->pos++;
            }

            (*dirent_count)++;
        }
    }

    return !END_READ;
}

int read_all_dirents_from_indirect_block(
    struct hodo_datablock* indirect_block,
    struct dir_context *ctx,
    uint64_t *dirent_count
) {
    // ZONEFS_TRACE();

    logical_block_number_t temp_block_logical_number;
    struct hodo_datablock *temp_block = kmalloc(HODO_DATABLOCK_SIZE, GFP_KERNEL);

     if (temp_block == NULL) {
        // pr_info("zonefs: (error in hodo_sub_readdir) cannot allocate 4KB heap space for datablock variable\n");
        return 0;
    }

    for (int j = HODO_DATA_START; j < HODO_DATABLOCK_SIZE - BLOCK_PTR_SZ; j += BLOCK_PTR_SZ) {
        memcpy(&temp_block_logical_number, (void*)indirect_block + j, BLOCK_PTR_SZ);

        if(!is_block_logical_number_valid(temp_block_logical_number))
            continue;

        hodo_read_struct(temp_block_logical_number, temp_block, HODO_DATABLOCK_SIZE);

        uint64_t result;

        if(is_directblock(temp_block))
            result = read_all_dirents_from_direct_block(temp_block, ctx, dirent_count);

        else 
            result = read_all_dirents_from_indirect_block(temp_block, ctx, dirent_count);

        if (result == END_READ){
            kfree(temp_block);
            return result;
        }
    }

    kfree(temp_block);
    return !END_READ;
}

static bool hodo_dir_emit(struct dir_context *ctx, struct hodo_dirent *temp_dirent){
    return dir_emit(
                    ctx,
                    temp_dirent->name,
                    temp_dirent->name_len,
                    temp_dirent->i_ino,
                    ((temp_dirent->file_type == HODO_TYPE_DIR) ? DT_DIR : DT_REG)
    );
}

/*-------------------------------------------------------------create용 함수-------------------------------------------------------------------------------*/
int add_dirent(struct inode* dir, struct hodo_inode* sub_inode) {
    // ZONEFS_TRACE();

    // read directory inode
    logical_block_number_t dir_block_logical_number = dir->i_ino;
    struct hodo_inode dir_inode = {0,};

    hodo_read_struct(dir_block_logical_number, &dir_inode, sizeof(struct hodo_inode));

    struct hodo_datablock* temp_datablock = kmalloc(HODO_DATABLOCK_SIZE, GFP_KERNEL);

    for (int i = 0; i < 10; ++i) {
        // pr_info("%dth data block\n", i);
        if (dir_inode.direct[i] != 0) {
            logical_block_number_t temp_logical_number = dir_inode.direct[i];
            hodo_read_struct(temp_logical_number, temp_datablock, sizeof(struct hodo_datablock));

            for (int j = HODO_DATA_START; j < HODO_DATABLOCK_SIZE - sizeof(struct hodo_dirent); j += sizeof(struct hodo_dirent)) {
                struct hodo_dirent temp_dirent;
                memcpy(&temp_dirent, (void*)temp_datablock + j, sizeof(struct hodo_dirent));
                
                if(is_dirent_valid(&temp_dirent)) {
                    continue; 
                }
                else {
                    memcpy(temp_dirent.name, sub_inode->name, sub_inode->name_len);
                    temp_dirent.name_len = sub_inode->name_len;
                    temp_dirent.i_ino = sub_inode->i_ino;
                    temp_dirent.file_type = sub_inode->type;

                    memcpy((void*)temp_datablock + j, &temp_dirent, sizeof(struct hodo_dirent));

                    dir_inode.file_len++;
                    hodo_write_struct(temp_datablock, sizeof(struct hodo_datablock), &temp_logical_number);

                    hodo_write_struct(&dir_inode, sizeof(struct hodo_inode), &dir_block_logical_number);

                    kfree(temp_datablock);
                    return 0;
                }
            }
        }
        else {
            struct hodo_dirent temp_dirent;
            memcpy(temp_dirent.name, sub_inode->name, sub_inode->name_len);
            temp_dirent.name_len = sub_inode->name_len;
            temp_dirent.i_ino = sub_inode->i_ino;
            temp_dirent.file_type = sub_inode->type;

            memset(temp_datablock, 0, HODO_DATABLOCK_SIZE);
            temp_datablock->magic[0] = 'D';
            temp_datablock->magic[1] = 'A';
            temp_datablock->magic[2] = 'T';
            temp_datablock->magic[3] = '0';

            memcpy((void*)temp_datablock + HODO_DATA_START, &temp_dirent, sizeof(struct hodo_dirent));

            dir_inode.file_len++;
            logical_block_number_t temp_logical_number = 0;
            hodo_write_struct(temp_datablock, sizeof(struct hodo_datablock), &temp_logical_number);

            dir_inode.direct[i] = temp_logical_number;
            hodo_write_struct(&dir_inode, sizeof(struct hodo_inode), &dir_block_logical_number);

            kfree(temp_datablock);
            return 0;
        }
    }

    kfree(temp_datablock);
    return -1;
}

/*-------------------------------------------------------------unlink용 함수-------------------------------------------------------------------------------*/
int remove_dirent(struct hodo_inode *dir_hodo_inode, struct inode *dir, const char *target_name, logical_block_number_t *out_logical_number){
    // ZONEFS_TRACE();

    struct hodo_datablock *buf_block = kmalloc(HODO_DATABLOCK_SIZE, GFP_KERNEL);

    if (buf_block == NULL) {
        // pr_info("zonefs: (error in hodo_unlink) cannot allocate 4KB heap space for datablock variable\n");
        return NOTHING_FOUND;
    }

    int result;
    logical_block_number_t written_logical_number;

    //direct data block에서 target_mapping_index를 가진 dirent를 찾아 지우기
    logical_block_number_t direct_block_logical_number;

    for (int i = 0; i < 10; i++) {
        direct_block_logical_number = dir_hodo_inode->direct[i];
        
        if(is_block_logical_number_valid(direct_block_logical_number)) {
            hodo_read_struct(direct_block_logical_number, buf_block, HODO_DATABLOCK_SIZE);

            written_logical_number = direct_block_logical_number;
            result = remove_dirent_from_direct_block(buf_block, target_name, &written_logical_number);
            
            if(result != NOTHING_FOUND) {
                //dirent를 삭제하면서 direct_datablock가 새로 써지므로, 이를 가리키는 hodo_inode는 새로 써져야 한다.
                dir_hodo_inode->direct[i] = written_logical_number;
                dir_hodo_inode->i_atime = current_time(dir);
                dir_hodo_inode->i_mtime = current_time(dir);
                dir_hodo_inode->i_ctime = current_time(dir);
                
                //dirent가 삭제되면서 예하 파일 수가 줄어들었으므로, 이를 반영한다
                dir_hodo_inode->file_len--;

                hodo_write_struct(dir_hodo_inode, sizeof(struct hodo_inode), out_logical_number);


                hodo_write_struct(dir_hodo_inode, sizeof(struct hodo_inode), out_logical_number);

                kfree(buf_block);
                return result;
            }
        }
    }

    //single, double, triple indirect data block들이 가리키는 direct_data_block에서 target_mapping_index를 가진 dirent를 찾아 지우기
    logical_block_number_t indirect_block_logical_number[3] = {
        dir_hodo_inode->single_indirect,
        dir_hodo_inode->double_indirect,
        dir_hodo_inode->triple_indirect
    };

    for(int i = 0; i < 3; i++){
        if(is_block_logical_number_valid(indirect_block_logical_number[i])) {
            hodo_read_struct(indirect_block_logical_number[i], buf_block, HODO_DATABLOCK_SIZE);

            written_logical_number = indirect_block_logical_number[i];
            result = remove_dirent_from_indirect_block(buf_block, target_name, &written_logical_number);

            if(result != NOTHING_FOUND) {
                //dirent를 삭제하면서 direct_datablock가 새로 써지고, 이를 가리키는 indirect_datablock도 새로 써지므로, 이를 가리키는 hodo_inode 또한 새로 써져야 한다.
                indirect_block_logical_number[i] = written_logical_number;
                dir_hodo_inode->i_atime = current_time(dir);
                dir_hodo_inode->i_mtime = current_time(dir);
                dir_hodo_inode->i_ctime = current_time(dir);

                //dirent가 삭제되면서 예하 파일 수가 줄어들었으므로, 이를 반영한다
                dir_hodo_inode->file_len--;

                hodo_write_struct(dir_hodo_inode, sizeof(struct hodo_inode), out_logical_number);
                kfree(buf_block);
                return result;
            }
        }
    }

    //해당 이름의 dirent를 찾아서 삭제하지 못하였으므로
    kfree(buf_block);
    return NOTHING_FOUND;
}

int remove_dirent_from_direct_block(
    struct hodo_datablock *direct_block,
    const char *target_name,
    logical_block_number_t *out_logical_number
) {
    // ZONEFS_TRACE();

    for (int j = HODO_DATA_START; j < HODO_DATABLOCK_SIZE - sizeof(struct hodo_dirent); j += sizeof(struct hodo_dirent)) {
        struct hodo_dirent temp_dirent;
        memcpy(&temp_dirent, (void*)direct_block + j, sizeof(struct hodo_dirent));

        if (memcmp(temp_dirent.name, target_name, HODO_MAX_NAME_LEN) == 0){
            compact_datablock(direct_block, j, sizeof(struct hodo_dirent), out_logical_number);

            return !NOTHING_FOUND;
        }
    }

    return NOTHING_FOUND;
}

int remove_dirent_from_indirect_block(
    struct hodo_datablock *indirect_block,
    const char *target_name,
    logical_block_number_t *out_logical_number
) {
    // ZONEFS_TRACE();

    logical_block_number_t temp_block_logical_number;
    struct hodo_datablock *temp_block = kmalloc(HODO_DATABLOCK_SIZE, GFP_KERNEL);

    if (temp_block == NULL) {
        // pr_info("zonefs: (error in hodo_unlink) cannot allocate 4KB heap space for datablock variable\n");
        return NOTHING_FOUND;
    }

    for (int j = HODO_DATA_START; j < HODO_DATABLOCK_SIZE - BLOCK_PTR_SZ; j += BLOCK_PTR_SZ) {
        memcpy(&temp_block_logical_number, (void*)indirect_block + j, BLOCK_PTR_SZ);

        if(!is_block_logical_number_valid(temp_block_logical_number))  
            continue;

        hodo_read_struct(temp_block_logical_number, temp_block, HODO_DATABLOCK_SIZE);

        int result;
        logical_block_number_t written_logical_number = temp_block_logical_number;

        if(is_directblock(temp_block))
            result = remove_dirent_from_direct_block(temp_block, target_name, &written_logical_number);

        else 
            result = remove_dirent_from_indirect_block(temp_block, target_name, &written_logical_number);

        if (result != NOTHING_FOUND && is_block_logical_number_valid(written_logical_number)){
            //dirent를 삭제하면서 direct_datablock가 새로 써지므로, 이를 가리키는 indirect_datablock도 거슬러 올라가며 새로 써져야 한다.
            memcpy((void*)indirect_block + j, &written_logical_number, BLOCK_PTR_SZ);
            hodo_write_struct(indirect_block, sizeof(struct hodo_datablock), out_logical_number);

            kfree(temp_block);
            return result;
        }
    }

    kfree(temp_block);
    return NOTHING_FOUND;
}

/*-------------------------------------------------------------rmdir용 함수 선언--------------------------------------------------------------------------------*/
bool check_directory_empty(struct dentry *dentry){
    // ZONEFS_TRACE();
    
    //디렉토리가 루트 디록테리면 매핑 테이블에서 인덱스를 아이노드 번호가 아니라, 0번을 이용해야 하므로 루트 디렉토리인지를 확인한다.
    uint64_t dir_mapping_index;

    if(dentry->d_inode == dentry->d_sb->s_root->d_inode) {
        dir_mapping_index = mapping_info.starting_logical_number;
    }
    else {
        dir_mapping_index = dentry->d_inode->i_ino;
    }

    //디렉토리의 hodo 아이노드를 저장장치로부터 읽어온다
    logical_block_number_t dir_hodo_logical_number= dir_mapping_index;
    struct hodo_inode dir_hodo_inode;
    hodo_read_struct(dir_hodo_logical_number, &dir_hodo_inode, sizeof(struct hodo_inode));

    //디렉토리 hodo 아이노드가 가리키는 데이터블록들을 순회할 준비를 한다
    struct hodo_datablock *buf_block = kmalloc(HODO_DATABLOCK_SIZE, GFP_KERNEL);

    if (buf_block == NULL) {
        // pr_info("zonefs: (error in hodo_sub_readdir) cannot allocate 4KB heap space for datablock variable\n");
        return EMPTY_CHECKED;
    }

    int result;

    //direct data block들이 비워져있는지 확인하기
    logical_block_number_t direct_block_logical_number;

    for (int i = 0; i < 10; i++) {
        direct_block_logical_number = dir_hodo_inode.direct[i];
        
        if(is_block_logical_number_valid(direct_block_logical_number)) {

            hodo_read_struct(direct_block_logical_number, buf_block, HODO_DATABLOCK_SIZE);

            result = check_directory_empty_from_direct_block(buf_block);
            
            if(result != EMPTY_CHECKED) {
                kfree(buf_block);
                return result;
            }
        }
    }

    //single, double, triple indirect data block를 통해 간접적으로 가리키는 direct_data_block들이 비워져있는지 확인하기
    logical_block_number_t indirect_block_logical_number[3] = {
        dir_hodo_inode.single_indirect,
        dir_hodo_inode.double_indirect,
        dir_hodo_inode.triple_indirect
    };

    for(int i = 0; i < 3; i++){
        if(is_block_logical_number_valid(indirect_block_logical_number[i])) {
            hodo_read_struct(indirect_block_logical_number[i], buf_block, HODO_DATABLOCK_SIZE);

            result = check_directory_empty_from_indirect_block(buf_block);

            if(result != EMPTY_CHECKED) {
                kfree(buf_block);
                return result;
            }
        }
    }

    //모두 잘 다 뒤져보았으므로 더 읽을 필요가 없음을 알려주기 위해 EMPTY_CHECKED를 반환하고 끝낸다
    kfree(buf_block);
    return EMPTY_CHECKED;
}

bool check_directory_empty_from_direct_block(struct hodo_datablock *direct_block){
    // ZONEFS_TRACE();

    const char zero[HODO_DATABLOCK_SIZE - HODO_DATA_START] = {0};

    if(memcmp(direct_block->data, zero, HODO_DATABLOCK_SIZE - HODO_DATA_START) == 0)
        return EMPTY_CHECKED;
    else
        return !EMPTY_CHECKED;
}

bool check_directory_empty_from_indirect_block(struct hodo_datablock *indirect_block){
    // ZONEFS_TRACE();

    logical_block_number_t temp_block_logical_number;
    struct hodo_datablock *temp_block = kmalloc(HODO_DATABLOCK_SIZE, GFP_KERNEL);

    if (temp_block == NULL) {
        // pr_info("zonefs: (error in hodo_sub_lookup) cannot allocate 4KB heap space for datablock variable\n");
        return EMPTY_CHECKED;
    }

    for (int j = HODO_DATA_START; j < HODO_DATABLOCK_SIZE - BLOCK_PTR_SZ; j += BLOCK_PTR_SZ) {
        memcpy(&temp_block_logical_number, (void*)indirect_block + j, BLOCK_PTR_SZ);

        if(!is_block_logical_number_valid(temp_block_logical_number))
            continue;

        hodo_read_struct(temp_block_logical_number, temp_block, HODO_DATABLOCK_SIZE);

        uint64_t result;

        if(is_directblock(temp_block))
            result = check_directory_empty_from_direct_block(temp_block);

        else 
            result = check_directory_empty_from_indirect_block(temp_block);

        if (result != EMPTY_CHECKED){
            kfree(temp_block);
            return result;
        }
    }

    kfree(temp_block);
    return EMPTY_CHECKED;
}

/*-------------------------------------------------------------비트맵용 함수-------------------------------------------------------------------------------*/
static void hodo_set_logical_bitmap(int i, int j) {
    mapping_info.logical_entry_bitmap[i] |= (1 << (31 - j));
}

static void hodo_unset_logical_bitmap(int i, int j) {
    mapping_info.logical_entry_bitmap[i] &= ~(1 << (31 - j));
}

int hodo_get_next_logical_number(void) {
    for (int i = 0; i < (NUMBER_MAPPING_TABLE_ENTRY / 32); ++i) {
        if (mapping_info.logical_entry_bitmap[i] != 0xFFFFFFFF) {
            for (int j = 0; j < 32; ++j) {
                if ((mapping_info.logical_entry_bitmap[i] & (1 << (31 - j))) == 0) {
                    hodo_set_logical_bitmap(i, j);
                    // pr_info("return logical number : %d\n", mapping_info.starting_logical_number + (i * 32 + j));
                    return mapping_info.starting_logical_number + (i * 32 + j);
                }
            }
        }
    }
    return -1;
}

static void hodo_set_GC_bitmap(struct hodo_block_pos physical_address) {
    int zone_id = physical_address.zone_id;
    int block_index = physical_address.block_index;

    //pr_info("set zone is over one : %d\n", zone_id);
    if(zone_id == 1) zone_1_valid_set_count++;
    else if(zone_id == 2) zone_2_valid_set_count++;

    mapping_info.GC_bitmap[zone_id][block_index / 32] |= (1 << (31 - (block_index % 32)));
}

static void hodo_unset_GC_bitmap(struct hodo_block_pos physical_address) {
    int zone_id = physical_address.zone_id;
    int block_index = physical_address.block_index;


    if(zone_id == 1) zone_1_valid_unset_count++;
    else if(zone_id == 2) zone_2_valid_unset_count++;

    mapping_info.GC_bitmap[zone_id][block_index / 32] &= ~(1 << (31 - (block_index % 32)));
}

static struct hodo_block_pos hodo_get_next_GC_valid(void) {
    struct hodo_block_pos start_pos = mapping_info.wp;
    struct hodo_block_pos ret = {0,0}; 

    for (int i = start_pos.zone_id; i < NUMBER_ZONES - 2; ++i) {
        for (int j = 0; j < (BLOCKS_PER_ZONE / 32); ++j) {
            if (mapping_info.GC_bitmap[i][j] != 0x00000000) {
                for (int k = 0; k < 32; ++k) {
                    if ((mapping_info.GC_bitmap[i][j] & (1 << (31 - k))) != 0) {
                        ret.zone_id = i;
                        ret.block_index = (j * 32) + k;
                        // pr_info("GC bitmap return value: (%d,%d)\n", ret.zone_id, ret.block_index);
                        if(ret.zone_id == 1) zone_1_valid_count++;
                        else if(ret.zone_id == 2) zone_2_valid_count++;
                        //pr_info("count is : %d / set zone number : %d\n", valid_cnt++, ret.zone_id);
                        return ret;
                    }
                }
            }
        }
    }

    return ret;
}

int hodo_erase_table_entry(int table_entry_index) {
    mapping_info.mapping_table[table_entry_index - mapping_info.starting_logical_number].zone_id = 0;  // check invalid
    hodo_unset_logical_bitmap(table_entry_index/32, table_entry_index%32);
    return 0;
}

/*-------------------------------------------------------------입출력 함수-------------------------------------------------------------------------------*/
ssize_t hodo_read_struct(logical_block_number_t logical_block_number, void *out_buf, size_t len) {
    // ZONEFS_TRACE();
    
    struct hodo_block_pos block_pos = mapping_info.mapping_table[logical_block_number - mapping_info.starting_logical_number];

    uint32_t zone_id = block_pos.zone_id;
    uint64_t offset = block_pos.block_index * HODO_DATABLOCK_SIZE;

    if (!out_buf || len == 0 || len > HODO_DATABLOCK_SIZE)
        return -EINVAL;

    //seq 파일을 열기 위해 경로 이름(path) 만들기
    const char path_up[16];
    char path_down[6] = {0, };

    memcpy(path_up, mount_point_path, sizeof(path_up));
    strcat(path_up, "/seq/");
    snprintf(path_down, sizeof(path_down), "%d", zone_id);
    const char *path = strncat(path_up, path_down, 6);

    struct file *zone_file;
    struct kiocb kiocb;
    struct iov_iter iter;
    struct kvec kvec;
    ssize_t ret;

    //파일 열기
    zone_file = filp_open(path, O_RDONLY | O_LARGEFILE, 0);
    if (IS_ERR(zone_file)) {
        pr_err("zonefs: filp_open(%s) failed\n", path);
        return PTR_ERR(zone_file);
    }

    //iov_iter 구성(읽을 버퍼들 여러개와 읽기 연산 등을 묶은 구조체)
    kvec.iov_base = out_buf;
    kvec.iov_len = len;
    iov_iter_kvec(&iter, ITER_DEST, &kvec, 1, len);

    //kiocb 구성(읽기 요청의 컨텍스트 정보)
    init_sync_kiocb(&kiocb, zone_file);
    kiocb.ki_pos = offset;
    kiocb.ki_flags = IOCB_DIRECT;

    //위 두 정보를 가지고 read_iter 실행
    if (!(zone_file->f_op) || !(zone_file->f_op->read_iter)) {
        pr_err("zonefs: read_iter not available on file\n");
        ret = -EINVAL;
        return ret;
    }

    ret = zone_file->f_op->read_iter(&kiocb, &iter);

    filp_close(zone_file, NULL);
    return ret;
}

ssize_t hodo_write_struct(void *buf, size_t len, logical_block_number_t *logical_block_number) {
    // ZONEFS_TRACE();

    uint32_t zone_id = mapping_info.wp.zone_id;
    uint64_t offset = mapping_info.wp.block_index * HODO_DATABLOCK_SIZE;

    if (!buf || len == 0 || len > HODO_DATABLOCK_SIZE)
        return -EINVAL;

    hodo_set_GC_bitmap(mapping_info.wp);
    if(*logical_block_number == 0){
        *logical_block_number = hodo_get_next_logical_number();
    }
    else {  // unset GC bitmap
        struct hodo_block_pos invalid_pos = mapping_info.mapping_table[*logical_block_number - mapping_info.starting_logical_number];
        hodo_unset_GC_bitmap(invalid_pos);
    }

    mapping_info.mapping_table[*logical_block_number - mapping_info.starting_logical_number] = mapping_info.wp;

    if (((char*)buf)[0] == 'D' && ((char*)buf)[1] == 'A' && ((char*)buf)[2] == 'T') {
        // pr_info("logical block number: %d\n", *logical_block_number);
        ((struct hodo_datablock*)buf)->logical_block_number = *logical_block_number;
    }

    //seq 파일을 열기 위해 경로 이름(path) 만들기
    const char path_up[16];
    char path_down[6] = {0, };

    memcpy(path_up, mount_point_path, sizeof(path_up));
    strcat(path_up, "/seq/");
    snprintf(path_down, sizeof(path_down), "%d", zone_id);
    const char *path = strncat(path_up, path_down, 6);

    struct file *zone_file;
    struct kiocb kiocb;
    struct iov_iter iter;
    struct kvec kvec;
    ssize_t ret;

    //파일 열기
    zone_file = filp_open(path, O_WRONLY | O_LARGEFILE, 0);
    if (IS_ERR(zone_file)) {
        pr_err("zonefs: filp_open(%s) failed\n", path);
        return PTR_ERR(zone_file);
    }

    //iov_iter 구성
    kvec.iov_base = (void*)buf;
    kvec.iov_len = len;
    iov_iter_kvec(&iter, ITER_SOURCE, &kvec, 1, len);

    //kiocb 구성
    init_sync_kiocb(&kiocb, zone_file);
    kiocb.ki_pos = offset;
    kiocb.ki_flags = IOCB_DIRECT;

    // pr_info("path: %s\toffset: %ld\n", path, offset);
    //위 두 정보를 가지고 write_iter 실행
    if (!(zone_file->f_op) || !(zone_file->f_op->write_iter)) {
        pr_err("zonefs: read_iter not available on file\n");
        filp_close(zone_file, NULL);
        ret = -EINVAL;
    }

    ret = zone_file->f_op->write_iter(&kiocb, &iter);
    filp_close(zone_file, NULL);

    if (offset + len == hodo_zone_size) {
        pr_info("hodo_write_struct wp is moved!\n");
        mapping_info.wp.zone_id = zone_id + 1; 
        mapping_info.wp.block_index = 0;
    }
    else {
        mapping_info.wp.zone_id = zone_id; 
        mapping_info.wp.block_index += 1;
    }

    return ret;
}

static ssize_t hodo_GC_write_struct(void *buf, size_t len, logical_block_number_t *logical_block_number) {
    // // ZONEFS_TRACE();

    uint32_t zone_id = mapping_info.swap_wp.zone_id;
    uint64_t offset = mapping_info.swap_wp.block_index * HODO_DATABLOCK_SIZE;

    if (!buf || len == 0 || len > HODO_DATABLOCK_SIZE)
        return -EINVAL;

    hodo_set_GC_bitmap(mapping_info.swap_wp);
    struct hodo_block_pos invalid_pos = mapping_info.mapping_table[*logical_block_number - mapping_info.starting_logical_number];
    hodo_unset_GC_bitmap(invalid_pos);

    mapping_info.mapping_table[*logical_block_number - mapping_info.starting_logical_number] = mapping_info.swap_wp;

    //seq 파일을 열기 위해 경로 이름(path) 만들기
    const char path_up[16];
    char path_down[6] = {0, };

    memcpy(path_up, mount_point_path, sizeof(path_up));
    strcat(path_up, "/seq/");
    snprintf(path_down, sizeof(path_down), "%d", zone_id);
    const char *path = strncat(path_up, path_down, 6);

    struct file *zone_file;
    struct kiocb kiocb;
    struct iov_iter iter;
    struct kvec kvec;
    ssize_t ret;

    //파일 열기
    zone_file = filp_open(path, O_WRONLY | O_LARGEFILE, 0);
    if (IS_ERR(zone_file)) {
        pr_err("zonefs: filp_open(%s) failed\n", path);
        return PTR_ERR(zone_file);
    }

    //iov_iter 구성
    kvec.iov_base = (void*)buf;
    kvec.iov_len = len;
    iov_iter_kvec(&iter, ITER_SOURCE, &kvec, 1, len);

    //kiocb 구성
    init_sync_kiocb(&kiocb, zone_file);
    kiocb.ki_pos = offset;
    kiocb.ki_flags = IOCB_DIRECT;

    // pr_info("path: %s\toffset: %ld\n", path, offset);
    //위 두 정보를 가지고 write_iter 실행
    if (!(zone_file->f_op) || !(zone_file->f_op->write_iter)) {
        pr_err("zonefs: read_iter not available on file\n");
        filp_close(zone_file, NULL);
        ret = -EINVAL;
    }

    ret = zone_file->f_op->write_iter(&kiocb, &iter);
    filp_close(zone_file, NULL);

    if (offset + len != hodo_zone_size) {
        mapping_info.swap_wp.zone_id = zone_id; 
        mapping_info.swap_wp.block_index += 1;
    }

    return ret;
}

static ssize_t hodo_GC_read_struct(struct hodo_block_pos block_pos, void *out_buf, size_t len) {
    // ZONEFS_TRACE();

    uint32_t zone_id = block_pos.zone_id;
    uint64_t offset = block_pos.block_index * HODO_DATABLOCK_SIZE;

    if (!out_buf || len == 0 || len > HODO_DATABLOCK_SIZE)
        return -EINVAL;

    //seq 파일을 열기 위해 경로 이름(path) 만들기
    const char path_up[16];
    char path_down[6] = {0, };

    memcpy(path_up, mount_point_path, sizeof(path_up));
    strcat(path_up, "/seq/");
    snprintf(path_down, sizeof(path_down), "%d", zone_id);
    const char *path = strncat(path_up, path_down, 6);

    struct file *zone_file;
    struct kiocb kiocb;
    struct iov_iter iter;
    struct kvec kvec;
    ssize_t ret;

    //파일 열기
    zone_file = filp_open(path, O_RDONLY | O_LARGEFILE, 0);
    if (IS_ERR(zone_file)) {
        pr_err("zonefs: filp_open(%s) failed\n", path);
        return PTR_ERR(zone_file);
    }

    //iov_iter 구성(읽을 버퍼들 여러개와 읽기 연산 등을 묶은 구조체)
    kvec.iov_base = out_buf;
    kvec.iov_len = len;
    iov_iter_kvec(&iter, ITER_DEST, &kvec, 1, len);

    //kiocb 구성(읽기 요청의 컨텍스트 정보)
    init_sync_kiocb(&kiocb, zone_file);
    kiocb.ki_pos = offset;
    kiocb.ki_flags = IOCB_DIRECT;

    //위 두 정보를 가지고 read_iter 실행
    if (!(zone_file->f_op) || !(zone_file->f_op->read_iter)) {
        pr_err("zonefs: read_iter not available on file\n");
        ret = -EINVAL;
        return ret;
    }

    ret = zone_file->f_op->read_iter(&kiocb, &iter);

    filp_close(zone_file, NULL);
    return ret;
}

ssize_t compact_datablock(struct hodo_datablock *source_block, int remove_start_index, int remove_size, logical_block_number_t *out_logical_number){
    struct hodo_datablock *temp_block = kmalloc(HODO_DATABLOCK_SIZE, GFP_KERNEL);
    int remove_end_index = remove_start_index + remove_size;

    //(0~remove_start_index) 사이의 내용을 temp_block으로 옮긴다
    memcpy(temp_block, source_block, remove_start_index);
    //(remove_end_index~HODO_DATABLOCK_SIZE) 사이의 내용을 temp_block 안에 덧붙인다.
    memcpy((void*)temp_block + remove_start_index, (void*)source_block + remove_end_index, HODO_DATABLOCK_SIZE - remove_end_index);

    ssize_t size = hodo_write_struct(temp_block, sizeof(struct hodo_datablock), out_logical_number);
    
    kfree(temp_block);
    return size;
}

ssize_t hodo_read_on_disk_mapping_info(void) {
    // ZONEFS_TRACE();

    uint32_t zone_id = 0;
    uint64_t offset = 0;

    const char path_up[16];
    char path_down[6] = {0, };

    memcpy(path_up, mount_point_path, sizeof(path_up));
    strcat(path_up, "/seq/");
    snprintf(path_down, sizeof(path_down), "%d", zone_id);
    const char *path = strncat(path_up, path_down, 6);

    struct file *zone_file;
    struct kiocb kiocb;
    struct iov_iter iter;
    struct kvec kvec;
    ssize_t ret;

    //파일 열기
    zone_file = filp_open(path, O_RDONLY | O_LARGEFILE, 0);
    if (IS_ERR(zone_file)) {
        pr_err("zonefs: filp_open(%s) failed\n", path);
        return PTR_ERR(zone_file);
    }

    //iov_iter 구성(읽을 버퍼들 여러개와 읽기 연산 등을 묶은 구조체)
    kvec.iov_base = &mapping_info;
    kvec.iov_len = sizeof(struct hodo_mapping_info);
    iov_iter_kvec(&iter, ITER_DEST, &kvec, 1, sizeof(struct hodo_mapping_info));

    //kiocb 구성(읽기 요청의 컨텍스트 정보)
    init_sync_kiocb(&kiocb, zone_file);
    kiocb.ki_pos = offset;
    kiocb.ki_flags = IOCB_DIRECT;

    //위 두 정보를 가지고 read_iter 실행
    if (!(zone_file->f_op) || !(zone_file->f_op->read_iter)) {
         pr_err("zonefs: read_iter not available on file\n");
        ret = -EINVAL;
    }

    ret = zone_file->f_op->read_iter(&kiocb, &iter);

    filp_close(zone_file, NULL);
    return ret;
}

/*-------------------------------------------------------------도구 함수-------------------------------------------------------------------------------*/
bool is_dirent_valid(struct hodo_dirent *dirent){
    if(
        dirent->name[0] != '\0' &&
        dirent->name_len != 0 &&
        dirent->i_ino != 0 &&
        (dirent->file_type == HODO_TYPE_DIR ||
        dirent->file_type == HODO_TYPE_REG)
    )   
        return true;
    else
        return false;
}

bool is_block_logical_number_valid(logical_block_number_t logical_block_number){
    if(logical_block_number != 0) return true;
    else return false;
}

bool is_directblock(struct hodo_datablock *datablock){
    if(datablock->magic[3] == '0') return true;
    else return false;
}
