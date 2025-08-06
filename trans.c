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
#include "zonefs.h"
#include "hodo.h"
#include "trans.h"
/*-------------------------------------------------------------static 함수 선언-------------------------------------------------------------------------------*/
static bool hodo_dir_emit(struct dir_context *ctx, struct hodo_dirent *temp_dirent);
static void hodo_set_bitmap(int i, int j);
static void hodo_unset_bitmap(int i, int j);

/*-------------------------------------------------------------lookup용 함수----------------------------------------------------------------------------------*/
uint64_t find_inode_number(struct hodo_inode *dir_hodo_inode, const char *target_name) {
    ZONEFS_TRACE();

    uint64_t result;
    struct hodo_datablock *buf_block = kmalloc(HODO_DATABLOCK_SIZE, GFP_KERNEL);

    if (buf_block == NULL) {
        pr_info("zonefs: (error in hodo_sub_lookup) cannot allocate 4KB heap space for datablock variable\n");
        return NOTHING_FOUND;
    }

    //direct data block들에서 특정 이름의 hodo dentry를 찾아보기
    struct hodo_block_pos direct_block_pos;

    for (int i = 0; i < 10; i++) {
        direct_block_pos = dir_hodo_inode->direct[i];
        
        if(is_block_pos_valid(direct_block_pos)) {
            hodo_read_struct(direct_block_pos, buf_block, HODO_DATABLOCK_SIZE);

            result = find_inode_number_from_direct_block(buf_block, target_name);

            if (result != NOTHING_FOUND) {
                kfree(buf_block);
                return result;
            }
        }
    }

    //single, double, triple indirect data block에서 특정 이름의 hodo dentry를 찾아보기
    struct hodo_block_pos indirect_block_pos[3] = {
        dir_hodo_inode->single_indirect,
        dir_hodo_inode->double_indirect,
        dir_hodo_inode->triple_indirect
    };

    for(int i = 0; i < 3; i++){
        if(is_block_pos_valid(indirect_block_pos[i])) {
            hodo_read_struct(indirect_block_pos[i], buf_block, HODO_DATABLOCK_SIZE);

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
    ZONEFS_TRACE();

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
    ZONEFS_TRACE();

    struct hodo_block_pos temp_block_pos;
    struct hodo_datablock *temp_block = kmalloc(HODO_DATABLOCK_SIZE, GFP_KERNEL);

    if (temp_block == NULL) {
        pr_info("zonefs: (error in hodo_sub_lookup) cannot allocate 4KB heap space for datablock variable\n");
        return NOTHING_FOUND;
    }

    for (int j = HODO_DATA_START; j < HODO_DATABLOCK_SIZE - sizeof(struct hodo_block_pos); j += sizeof(struct hodo_block_pos)) {
        memcpy(&temp_block_pos, (void*)indirect_block + j, sizeof(struct hodo_block_pos));

        if(!is_block_pos_valid(temp_block_pos))
            continue;

        hodo_read_struct(temp_block_pos, temp_block, HODO_DATABLOCK_SIZE);

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
    ZONEFS_TRACE();

    struct hodo_datablock *buf_block = kmalloc(HODO_DATABLOCK_SIZE, GFP_KERNEL);

    if (buf_block == NULL) {
        pr_info("zonefs: (error in hodo_sub_readdir) cannot allocate 4KB heap space for datablock variable\n");
        return END_READ;
    }

    int result;

    //direct data block들에서 hodo dirent들을 읽어내기
    struct hodo_block_pos direct_block_pos;

    for (int i = 0; i < 10; i++) {
        direct_block_pos = dir_hodo_inode->direct[i];
        
        if(is_block_pos_valid(direct_block_pos)) {

            hodo_read_struct(direct_block_pos, buf_block, HODO_DATABLOCK_SIZE);

            result = read_all_dirents_from_direct_block(buf_block, ctx, dirent_count);
            
            if(result == END_READ) {
                kfree(buf_block);
                return result;
            }
        }
    }

    //single, double, triple indirect data block에서 hodo dirent들을 읽어내기
    struct hodo_block_pos indirect_block_pos[3] = {
        dir_hodo_inode->single_indirect,
        dir_hodo_inode->double_indirect,
        dir_hodo_inode->triple_indirect
    };

    for(int i = 0; i < 3; i++){
        if(is_block_pos_valid(indirect_block_pos[i])) {
            hodo_read_struct(indirect_block_pos[i], buf_block, HODO_DATABLOCK_SIZE);

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
    ZONEFS_TRACE();

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
    ZONEFS_TRACE();

    struct hodo_block_pos temp_block_pos;
    struct hodo_datablock *temp_block = kmalloc(HODO_DATABLOCK_SIZE, GFP_KERNEL);

     if (temp_block == NULL) {
        pr_info("zonefs: (error in hodo_sub_readdir) cannot allocate 4KB heap space for datablock variable\n");
        return 0;
    }

    for (int j = HODO_DATA_START; j < HODO_DATABLOCK_SIZE - sizeof(struct hodo_block_pos); j += sizeof(struct hodo_block_pos)) {
        memcpy(&temp_block_pos, (void*)indirect_block + j, sizeof(struct hodo_block_pos));

        if(!is_block_pos_valid(temp_block_pos))
            continue;

        hodo_read_struct(temp_block_pos, temp_block, HODO_DATABLOCK_SIZE);

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
    ZONEFS_TRACE();

    // read directory inode
    struct hodo_block_pos dir_block_pos = mapping_info.mapping_table[dir->i_ino - mapping_info.starting_ino];
    struct hodo_inode dir_inode = {0,};

    hodo_read_struct(dir_block_pos, &dir_inode, sizeof(struct hodo_inode));

    struct hodo_datablock* temp_datablock = kmalloc(HODO_DATABLOCK_SIZE, GFP_KERNEL);

    for (int i = 0; i < 10; ++i) {
        pr_info("%dth data block\n", i);
        if (dir_inode.direct[i].zone_id != 0) {
            struct hodo_block_pos temp_pos = {dir_inode.direct[i].zone_id, dir_inode.direct[i].offset};
            hodo_read_struct(temp_pos, temp_datablock, sizeof(struct hodo_datablock));

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

                    dir_inode.direct[i].zone_id = mapping_info.wp.zone_id;
                    dir_inode.direct[i].offset = mapping_info.wp.offset;
                    hodo_write_struct(temp_datablock, sizeof(struct hodo_datablock), NULL);

                    mapping_info.mapping_table[dir->i_ino - mapping_info.starting_ino].zone_id = mapping_info.wp.zone_id; 
                    mapping_info.mapping_table[dir->i_ino - mapping_info.starting_ino].offset = mapping_info.wp.offset; 
                    hodo_write_struct(&dir_inode, sizeof(struct hodo_inode), NULL);

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

            memcpy((void*)temp_datablock + 4, &temp_dirent, sizeof(struct hodo_dirent));

            dir_inode.direct[i].zone_id = mapping_info.wp.zone_id;
            dir_inode.direct[i].offset = mapping_info.wp.offset;
            hodo_write_struct(temp_datablock, sizeof(struct hodo_datablock), NULL);

            mapping_info.mapping_table[dir->i_ino - mapping_info.starting_ino].zone_id = mapping_info.wp.zone_id; 
            mapping_info.mapping_table[dir->i_ino - mapping_info.starting_ino].offset = mapping_info.wp.offset; 
            hodo_write_struct(&dir_inode, sizeof(struct hodo_inode), NULL);

            kfree(temp_datablock);
            return 0;
        }
    }

    kfree(temp_datablock);
    return -1;
}

/*-------------------------------------------------------------unlink용 함수-------------------------------------------------------------------------------*/
int remove_dirent(struct hodo_inode *dir_hodo_inode, struct inode *dir, const char *target_name, struct hodo_block_pos *out_pos){
    ZONEFS_TRACE();

    struct hodo_datablock *buf_block = kmalloc(HODO_DATABLOCK_SIZE, GFP_KERNEL);

    if (buf_block == NULL) {
        pr_info("zonefs: (error in hodo_unlink) cannot allocate 4KB heap space for datablock variable\n");
        return NOTHING_FOUND;
    }

    int result;
    struct hodo_block_pos written_pos;

    //direct data block에서 target_mapping_index를 가진 dirent를 찾아 지우기
    struct hodo_block_pos direct_block_pos;

    for (int i = 0; i < 10; i++) {
        direct_block_pos = dir_hodo_inode->direct[i];
        
        if(is_block_pos_valid(direct_block_pos)) {
            hodo_read_struct(direct_block_pos, buf_block, HODO_DATABLOCK_SIZE);

            result = remove_dirent_from_direct_block(buf_block, target_name, &written_pos);
            
            if(result != NOTHING_FOUND) {
                //dirent를 삭제하면서 direct_datablock가 새로 써지므로, 이를 가리키는 hodo_inode는 새로 써져야 한다.
                dir_hodo_inode->direct[i] = written_pos;
                dir_hodo_inode->i_atime = current_time(dir);
                dir_hodo_inode->i_mtime = current_time(dir);
                dir_hodo_inode->i_ctime = current_time(dir);
                
                hodo_write_struct(dir_hodo_inode, sizeof(struct hodo_inode), out_pos);

                kfree(buf_block);
                return result;
            }
        }
    }

    //single, double, triple indirect data block들이 가리키는 direct_data_block에서 target_mapping_index를 가진 dirent를 찾아 지우기
    struct hodo_block_pos *indirect_block_pos[3] = {
        &(dir_hodo_inode->single_indirect),
        &(dir_hodo_inode->double_indirect),
        &(dir_hodo_inode->triple_indirect)
    };

    for(int i = 0; i < 3; i++){
        if(is_block_pos_valid(*indirect_block_pos[i])) {
            hodo_read_struct(*indirect_block_pos[i], buf_block, HODO_DATABLOCK_SIZE);

            result = remove_dirent_from_indirect_block(buf_block, target_name, &written_pos);

            if(result != NOTHING_FOUND) {
                //dirent를 삭제하면서 direct_datablock가 새로 써지고, 이를 가리키는 indirect_datablock도 새로 써지므로, 이를 가리키는 hodo_inode 또한 새로 써져야 한다.
                *indirect_block_pos[i] = written_pos;
                dir_hodo_inode->i_atime = current_time(dir);
                dir_hodo_inode->i_mtime = current_time(dir);
                dir_hodo_inode->i_ctime = current_time(dir);

                hodo_write_struct(dir_hodo_inode, sizeof(struct hodo_inode), out_pos);
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
    struct hodo_block_pos *out_pos
) {
    ZONEFS_TRACE();

    for (int j = HODO_DATA_START; j < HODO_DATABLOCK_SIZE - sizeof(struct hodo_dirent); j += sizeof(struct hodo_dirent)) {
        struct hodo_dirent temp_dirent;
        memcpy(&temp_dirent, (void*)direct_block + j, sizeof(struct hodo_dirent));

        if (memcmp(temp_dirent.name, target_name, HODO_MAX_NAME_LEN) == 0){
            compact_datablock(direct_block, j, sizeof(struct hodo_dirent), out_pos);

            return !NOTHING_FOUND;
        }
    }

    return NOTHING_FOUND;
}

int remove_dirent_from_indirect_block(
    struct hodo_datablock *indirect_block,
    const char *target_name,
    struct hodo_block_pos *out_pos
) {
    ZONEFS_TRACE();

    struct hodo_block_pos temp_block_pos;
    struct hodo_datablock *temp_block = kmalloc(HODO_DATABLOCK_SIZE, GFP_KERNEL);

    if (temp_block == NULL) {
        pr_info("zonefs: (error in hodo_unlink) cannot allocate 4KB heap space for datablock variable\n");
        return NOTHING_FOUND;
    }

    for (int j = HODO_DATA_START; j < HODO_DATABLOCK_SIZE - sizeof(struct hodo_block_pos); j += sizeof(struct hodo_block_pos)) {
        memcpy(&temp_block_pos, (void*)indirect_block + j, sizeof(struct hodo_block_pos));

        if(!is_block_pos_valid(temp_block_pos))  
            continue;

        hodo_read_struct(temp_block_pos, temp_block, HODO_DATABLOCK_SIZE);

        int result;
        struct hodo_block_pos written_pos;

        if(is_directblock(temp_block))
            result = remove_dirent_from_direct_block(temp_block, target_name, &written_pos);

        else 
            result = remove_dirent_from_indirect_block(temp_block, target_name, &written_pos);

        if (result != NOTHING_FOUND && is_block_pos_valid(written_pos)){
            //dirent를 삭제하면서 direct_datablock가 새로 써지므로, 이를 가리키는 indirect_datablock도 거슬러 올라가며 새로 써져야 한다.
            memcpy((void*)indirect_block + j, &written_pos, sizeof(struct hodo_block_pos));
            hodo_write_struct(indirect_block, sizeof(struct hodo_datablock), out_pos);

            kfree(temp_block);
            return result;
        }
    }

    kfree(temp_block);
    return NOTHING_FOUND;
}

/*-------------------------------------------------------------rmdir용 함수 선언--------------------------------------------------------------------------------*/
bool check_directory_empty(struct dentry *dentry){
    ZONEFS_TRACE();
    
    //디렉토리가 루트 디록테리면 매핑 테이블에서 인덱스를 아이노드 번호가 아니라, 0번을 이용해야 하므로 루트 디렉토리인지를 확인한다.
    uint64_t dir_mapping_index;

    if(dentry->d_inode == dentry->d_sb->s_root->d_inode) {
        dir_mapping_index = 0;
    }
    else {
        dir_mapping_index = dentry->d_inode->i_ino - mapping_info.starting_ino;
    }

    //디렉토리의 hodo 아이노드를 저장장치로부터 읽어온다
    struct hodo_block_pos dir_hodo_pos = mapping_info.mapping_table[dir_mapping_index];
    struct hodo_inode dir_hodo_inode;
    hodo_read_struct(dir_hodo_pos, &dir_hodo_inode, sizeof(struct hodo_inode));

    //디렉토리 hodo 아이노드가 가리키는 데이터블록들을 순회할 준비를 한다
    struct hodo_datablock *buf_block = kmalloc(HODO_DATABLOCK_SIZE, GFP_KERNEL);

    if (buf_block == NULL) {
        pr_info("zonefs: (error in hodo_sub_readdir) cannot allocate 4KB heap space for datablock variable\n");
        return EMPTY_CHECKED;
    }

    int result;

    //direct data block들이 비워져있는지 확인하기
    struct hodo_block_pos direct_block_pos;

    for (int i = 0; i < 10; i++) {
        direct_block_pos = dir_hodo_inode.direct[i];
        
        if(is_block_pos_valid(direct_block_pos)) {

            hodo_read_struct(direct_block_pos, buf_block, HODO_DATABLOCK_SIZE);

            result = check_directory_empty_from_direct_block(buf_block);
            
            if(result != EMPTY_CHECKED) {
                kfree(buf_block);
                return result;
            }
        }
    }

    //single, double, triple indirect data block를 통해 간접적으로 가리키는 direct_data_block들이 비워져있는지 확인하기
    struct hodo_block_pos indirect_block_pos[3] = {
        dir_hodo_inode.single_indirect,
        dir_hodo_inode.double_indirect,
        dir_hodo_inode.triple_indirect
    };

    for(int i = 0; i < 3; i++){
        if(is_block_pos_valid(indirect_block_pos[i])) {
            hodo_read_struct(indirect_block_pos[i], buf_block, HODO_DATABLOCK_SIZE);

            result = check_directory_empty_from_indirect_block(buf_block);

            if(result != EMPTY_CHECKED) {
                kfree(buf_block);
                return result;
            }
        }
    }

    //모두 잘 다 뒤져보았으므로 더 읽을 필요가 없음을 알려주기 위해 END_READ를 반환하고 끝낸다
    kfree(buf_block);
    return EMPTY_CHECKED;
}

bool check_directory_empty_from_direct_block(struct hodo_datablock *direct_block){
    ZONEFS_TRACE();

    const char zero[HODO_DATABLOCK_SIZE - HODO_DATA_START] = {0};

    if(memcmp(direct_block->data, zero, HODO_DATABLOCK_SIZE - HODO_DATA_START) == 0)
        return EMPTY_CHECKED;
    else
        return !EMPTY_CHECKED;
}

bool check_directory_empty_from_indirect_block(struct hodo_datablock *indirect_block){
    ZONEFS_TRACE();

    struct hodo_block_pos temp_block_pos;
    struct hodo_datablock *temp_block = kmalloc(HODO_DATABLOCK_SIZE, GFP_KERNEL);

    if (temp_block == NULL) {
        pr_info("zonefs: (error in hodo_sub_lookup) cannot allocate 4KB heap space for datablock variable\n");
        return EMPTY_CHECKED;
    }

    for (int j = HODO_DATA_START; j < HODO_DATABLOCK_SIZE - sizeof(struct hodo_block_pos); j += sizeof(struct hodo_block_pos)) {
        memcpy(&temp_block_pos, (void*)indirect_block + j, sizeof(struct hodo_block_pos));

        if(!is_block_pos_valid(temp_block_pos))
            continue;

        hodo_read_struct(temp_block_pos, temp_block, HODO_DATABLOCK_SIZE);

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
static void hodo_set_bitmap(int i, int j) {
    mapping_info.bitmap[i] |= (1 << (31 - j));
}

static void hodo_unset_bitmap(int i, int j) {
    mapping_info.bitmap[i] &= ~(1 << (31 - j));
}

int hodo_get_next_ino(void) {
    for (int i = 0; i < (HODO_MAX_INODE / 32); ++i) {
        if (mapping_info.bitmap[i] != 0xFFFFFFFF) {
            for (int j = 0; j < 32; ++j) {
                if ((mapping_info.bitmap[i] & (1 << (31 - j))) == 0) {
                    hodo_set_bitmap(i, j);
                    return mapping_info.starting_ino + (i * 32 + j);
                }
            }
        }
    }
    return -1;
}

int hodo_erase_table_entry(int table_entry_index) {
    hodo_unset_bitmap(table_entry_index/32, table_entry_index%32);
    return 0;
}

/*-------------------------------------------------------------입출력 함수-------------------------------------------------------------------------------*/
ssize_t hodo_read_struct(struct hodo_block_pos block_pos, void *out_buf, size_t len) {
    ZONEFS_TRACE();

    uint32_t zone_id = block_pos.zone_id;
    uint64_t offset = block_pos.offset;

    //우리가 읽고 싶은 양은 0~HODO_DATABLOCK_SIZE Byte의 범위안에서 HODO_SECTOR_SIZE의 배수여야 한다
    if (!out_buf || len == 0 || len > HODO_DATABLOCK_SIZE || len % HODO_SECTOR_SIZE != 0)
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

ssize_t hodo_write_struct(void *buf, size_t len, struct hodo_block_pos *out_pos) {
    ZONEFS_TRACE();

    uint32_t zone_id = mapping_info.wp.zone_id;
    uint64_t offset = mapping_info.wp.offset;
    if(out_pos != NULL){
        out_pos->zone_id = zone_id;
        out_pos->offset = offset;
    }

    //write size는 HODO_SECTOR_SIZE 단위여야 하고, 하나의 블럭 이내의 크기여야 한다.
    if (!buf || len == 0 || len > HODO_DATABLOCK_SIZE || (len % HODO_SECTOR_SIZE != 0))
        return -EINVAL;

    if (offset + len >= hodo_zone_size) {
        if (zone_id + 1 > hodo_nr_zones) {
            pr_err("device is full\n");
            return 0;
        }

        zone_id++;
        offset = 0;
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

    pr_info("path: %s\toffset: %ld\n", path, offset);
    //위 두 정보를 가지고 write_iter 실행
    if (!(zone_file->f_op) || !(zone_file->f_op->write_iter)) {
         pr_err("zonefs: read_iter not available on file\n");
        ret = -EINVAL;
    }

    ret = zone_file->f_op->write_iter(&kiocb, &iter);

    filp_close(zone_file, NULL);

    if (offset + len == hodo_zone_size) {
        mapping_info.wp.zone_id = zone_id + 1; 
        mapping_info.wp.offset = 0;
    }
    else {
        mapping_info.wp.zone_id = zone_id; 
        mapping_info.wp.offset = offset + len;
    }

    return ret;
}

ssize_t compact_datablock(struct hodo_datablock *source_block, int remove_start_index, int remove_size, struct hodo_block_pos *out_pos){
    struct hodo_datablock *temp_block = kmalloc(HODO_DATABLOCK_SIZE, GFP_KERNEL);
    int remove_end_index = remove_start_index + remove_size;

    //(0~remove_start_index) 사이의 내용을 temp_block으로 옮긴다
    memcpy(temp_block, source_block, remove_start_index);
    //(remove_end_index~HODO_DATABLOCK_SIZE) 사이의 내용을 temp_block 안에 덧붙인다.
    memcpy((void*)temp_block + remove_start_index, (void*)source_block + remove_end_index, HODO_DATABLOCK_SIZE - remove_end_index);

    ssize_t size = hodo_write_struct(temp_block, sizeof(struct hodo_datablock), out_pos);
    
    kfree(temp_block);
    return size;
}

ssize_t hodo_read_on_disk_mapping_info(void) {
    ZONEFS_TRACE();

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

bool is_block_pos_valid(struct hodo_block_pos block_pos){
    if(block_pos.zone_id != 0) return true;
    else return false;
}

bool is_directblock(struct hodo_datablock *datablock){
    if(datablock->magic[3] == '0') return true;
    else return false;
}