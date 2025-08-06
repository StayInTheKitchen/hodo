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
#include <linux/quotaops.h>
#include <linux/string.h>

#include "zonefs.h"
#include "hodo.h"
#include "trans.h"
/*----------------------------------------------------------주소공간 오퍼레이션 함수 선언-------------------------------------------------------------------------------*/
//nothing

/*----------------------------------------------------------아이노드 오퍼레이션 함수 선언-------------------------------------------------------------------------------*/
static int hodo_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *iattr);
static struct dentry *hodo_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags);
static int hodo_create(struct mnt_idmap *idmap, struct inode *dir,struct dentry *dentry, umode_t mode, bool excl);
static int hodo_unlink(struct inode *dir, struct dentry *dentry);
static int hodo_mkdir(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode);
static int hodo_rmdir(struct inode *dir, struct dentry *dentry);

/*----------------------------------------------------------주소공간 오퍼레이션 함수 선언-------------------------------------------------------------------------------*/
//nothing

/*----------------------------------------------------------오퍼레이션 서브 함수 선언----------------------------------------------------------------------------------*/
static struct dentry *hodo_sub_lookup(struct inode* dir, struct dentry* dentry, unsigned int flags);
static int hodo_sub_readdir(struct file *file, struct dir_context *ctx);
static int hodo_sub_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *iattr);

/*----------------------------------------------------------글로벌 변수 및 초기화--------------------------------------------------------------------------------------*/
struct hodo_mapping_info mapping_info;
char mount_point_path[16];

void hodo_init(void) {
    ZONEFS_TRACE();

    // 마운트 포인트를 찾는 과정
    // /proc/self/mountinfo 파일을 파싱해서 zonefs가 마운트된 마운트 포인트를 찾는다
    // 한계: 마운트 포인트가 여러개라면...?
    struct file *filp;
    loff_t pos = 0;
    char *buf;
    ssize_t n;

    buf = kmalloc(4096, GFP_KERNEL);

    filp = filp_open("/proc/self/mountinfo", O_RDONLY, 0);
    if (IS_ERR(filp)) {
        printk(KERN_ERR "zonefs_parser: failed to open mountinfo\n");
        kfree(buf);
        return;
    }

    while ((n = kernel_read(filp, buf, 4096 - 1, &pos)) > 0) {
        buf[n] = '\0';

        char *line;
        while ((line = strsep(&buf, "\n")) != NULL) {
            if (strstr(line, "zonefs")) {
                char *sep = strstr(line, " - ");
                if (!sep)
                    continue;

                *sep = '\0';

                // 5th token = mountpoint
                char *token;
                int i = 0;
                char *mntpoint = NULL;

                while ((token = strsep(&line, " ")) != NULL) {
                    if (i++ == 4) {
                        mntpoint = token;
                        break;
                    }
                }

                if (mntpoint) {
                    memcpy(mount_point_path, mntpoint, sizeof(mount_point_path));
                    break;
                }
            }
        }
    }

    filp_close(filp, NULL);
    kfree(buf);

    hodo_read_on_disk_mapping_info();
    /*TO DO: crash check & recovery*/

    // if it's first mount after formatting
    // initialize in-memory mapping_info
    if (mapping_info.wp.zone_id == 0 && mapping_info.wp.offset == 0) {        
        // wp starts from (zone_id: 1, offset: 0)
        mapping_info.wp.zone_id = 1;
        mapping_info.wp.offset = 0;

        // starting_ino(root inode number) is always number of zones
        mapping_info.starting_ino = hodo_nr_zones;

        // root direcotry inode 설정
        struct hodo_inode root_inode;

        // 여기부터 hinode 초기화: 함수로 리팩터링
        root_inode.magic[0] = 'I';
        root_inode.magic[1] = 'N';
        root_inode.magic[2] = 'O';
        root_inode.magic[3] = 'D';

        root_inode.file_len = 0;

        root_inode.name_len = 1;
        memcpy(root_inode.name, "/", root_inode.name_len);

        root_inode.type = 1;

        root_inode.i_ino = hodo_get_next_ino();
        root_inode.i_mode = S_IFDIR; 

        root_inode.i_uid = current_fsuid();
        root_inode.i_gid = current_fsgid();

        root_inode.i_nlink = 1;

        // root inode를 wp에 쓰기
        mapping_info.mapping_table[root_inode.i_ino - mapping_info.starting_ino].zone_id = mapping_info.wp.zone_id; 
        mapping_info.mapping_table[root_inode.i_ino - mapping_info.starting_ino].offset = mapping_info.wp.offset;
        hodo_write_struct(&root_inode, sizeof(root_inode), NULL);
    }
}

/*----------------------------------------------------------파일 오퍼레이션 함수----------------------------------------------------------------------------------------*/
static int hodo_file_open(struct inode *inode, struct file *filp) {
    ZONEFS_TRACE();

    struct dentry *dentry = filp->f_path.dentry;
    const char *parent = dentry->d_parent->d_name.name;

    if (!strcmp(parent, "seq") || !strcmp(parent, "cnv")) {
        return zonefs_file_operations.open(inode, filp);
    }

    return 0;
}

static ssize_t hodo_read_dir(struct file *file, char __user *buf, size_t count, loff_t *pos) {
    ZONEFS_TRACE();

    return zonefs_dir_operations.read(file, buf, count, pos);
}

static int hodo_file_release(struct inode *inode, struct file *filp) {
    ZONEFS_TRACE();

    struct dentry *dentry = filp->f_path.dentry;
    const char *parent = dentry->d_parent->d_name.name;

    if (!strcmp(parent, "seq") || !strcmp(parent, "cnv")) {
        return zonefs_file_operations.release(inode, filp);
    }

    return 0;
}

static int hodo_file_fsync(struct file *filp, loff_t start, loff_t end, int datasync) {
    ZONEFS_TRACE();
    return zonefs_file_operations.fsync(filp, start, end, datasync);
}

static int hodo_file_mmap(struct file *filp, struct vm_area_struct *vma) {
    ZONEFS_TRACE();
    return zonefs_file_operations.mmap(filp, vma);
}

static loff_t hodo_file_llseek(struct file *filp, loff_t offset, int whence) {
    ZONEFS_TRACE();

    return zonefs_file_operations.llseek(filp, offset, whence);
}

static loff_t hodo_dir_llseek(struct file *filp, loff_t offset, int whence) {
    ZONEFS_TRACE();

    return zonefs_dir_operations.llseek(filp, offset, whence);
}

static ssize_t hodo_file_read_iter(struct kiocb *iocb, struct iov_iter *to) {
	ZONEFS_TRACE();
	return zonefs_file_operations.read_iter(iocb, to);
}

static ssize_t hodo_file_write_iter(struct kiocb *iocb, struct iov_iter *from) {
    ZONEFS_TRACE();
    return zonefs_file_operations.write_iter(iocb, from);
}

static ssize_t hodo_file_splice_read(struct file *in, loff_t *ppos,
                                              struct pipe_inode_info *pipe, size_t len, unsigned int flags) {
    ZONEFS_TRACE();
    return zonefs_file_operations.splice_read(in, ppos, pipe, len, flags);
}

static ssize_t hodo_file_splice_write(struct pipe_inode_info *pipe, struct file *out,
                                             loff_t *ppos, size_t len, unsigned int flags) {
    ZONEFS_TRACE();
    return zonefs_file_operations.splice_write(pipe, out, ppos, len, flags);
}

static int hodo_file_iocb_bio_iopoll(struct kiocb *iocb, struct io_comp_batch *iob, unsigned int flags) {
    ZONEFS_TRACE();
    return zonefs_file_operations.iopoll(iocb, iob, flags);
}

static int hodo_readdir(struct file *file, struct dir_context *ctx) {
    ZONEFS_TRACE();

    struct inode *inode = file_inode(file);
    struct dentry *dentry = file->f_path.dentry;
    const char *name = dentry->d_name.name;

    //보다 쉬운 디버깅을 위해, 'cnv', 'seq' 디렉토리에서 친 ls 명령어의 경우에는 기존의 zonefs readdir을 호출하도록 한다.
    if (!strcmp(name, "seq") || !strcmp(name, "cnv")) {
        pr_info("zonefs: readdir on 'seq' or 'cnv'\n");
        return zonefs_dir_operations.iterate_shared(file, ctx);
    }
    else {
        pr_info("zonefs: readdir on user directory\n");
        return hodo_sub_readdir(file, ctx);
    }
}

const struct file_operations hodo_file_operations = {
	.open		= hodo_file_open,
	.release	= hodo_file_release,
	.fsync		= hodo_file_fsync,
	.mmap		= hodo_file_mmap,
	.llseek		= hodo_file_llseek,
	.read_iter	= hodo_file_read_iter,
	.write_iter	= hodo_file_write_iter,
	.splice_read	= hodo_file_splice_read,
	.splice_write	= hodo_file_splice_write,
	.iopoll		= hodo_file_iocb_bio_iopoll,
};

const struct file_operations hodo_dir_operations = {
	.llseek		= hodo_dir_llseek,
	.read		= hodo_read_dir,
	.iterate_shared	= hodo_readdir,
};

/*-------------------------------------------------------------아이노드 오퍼레이션 함수-------------------------------------------------------------------------------*/
static int hodo_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *iattr) {
    ZONEFS_TRACE();

    const char *name = dentry->d_name.name;
    const char *parent = dentry->d_parent->d_name.name;

    if (!strcmp(name, "seq") || !strcmp(name, "cnv") ||
        !strcmp(parent, "seq") || !strcmp(parent, "cnv")) {
        pr_info("zonefs: using original setattr for '%s' (parent: %s)\n", name, parent);
        return zonefs_dir_inode_operations.setattr(idmap, dentry, iattr);
    }

    return hodo_sub_setattr(idmap, dentry, iattr);
}

static struct dentry *hodo_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags) {
    ZONEFS_TRACE();

    //seq, cnv 또는 그 하위 디렉토리는 기존 zonefs lookup 사용
    const char* name = dentry->d_name.name;
    const char* parent = dentry->d_parent->d_name.name;

    if (!strcmp(name, "seq") || !strcmp(name, "cnv") ||
        !strcmp(parent, "seq") || !strcmp(parent, "cnv")) {
        pr_info("zonefs: using original lookup for '%s' (parent: %s)\n", name, parent);
        return zonefs_dir_inode_operations.lookup(dir, dentry, flags);
    }

    //그 외는 우리가 정의한 hodo sub lookup 사용
    pr_info("zonefs: using custom lookup for '%s' (parent: %s)\n", name, parent);
    return hodo_sub_lookup(dir, dentry, flags);
}

static int hodo_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, bool excl) {
    ZONEFS_TRACE();

    struct inode *inode;
    struct timespec64 now;
    struct hodo_inode hinode;

    inode = new_inode(dir->i_sb);
    now = current_time(inode);

    // 여기부터 hinode 초기화: 함수로 리팩터링
    hinode.magic[0] = 'I';
    hinode.magic[1] = 'N';
    hinode.magic[2] = 'O';
    hinode.magic[3] = 'D';

    hinode.file_len = 0;

    hinode.name_len = dentry->d_name.len; 
    if (hinode.name_len > HODO_MAX_NAME_LEN) {
        // 구현해야 될 부분: error handling
    }
    memcpy(hinode.name, dentry->d_name.name, hinode.name_len);

    hinode.type = HODO_TYPE_REG;

    hinode.i_ino = hodo_get_next_ino();

    hinode.i_mode = S_IFREG | mode; 

    hinode.i_uid = current_fsuid();
    hinode.i_gid = current_fsgid();

    hinode.i_nlink = 1;

    hinode.i_atime = now;
    hinode.i_mtime = now;
    hinode.i_ctime = now;

    // BUG: zone에 걸친 write가 발생할 시, mapping table 관계가 내부에서 변해야 됨
    mapping_info.mapping_table[hinode.i_ino - mapping_info.starting_ino].zone_id = mapping_info.wp.zone_id; 
    mapping_info.mapping_table[hinode.i_ino - mapping_info.starting_ino].offset = mapping_info.wp.offset;
    hodo_write_struct(&hinode, sizeof(struct hodo_inode), NULL);
    // 여기까지 hinode 초기화: 함수로 리팩터링

    add_dirent(dir, &hinode);

    inode->i_ino  = hinode.i_ino;
    inode->i_sb   = dir->i_sb;
    inode->i_op   = &hodo_file_inode_operations;
    inode->i_fop  = &hodo_file_operations;
    inode->i_mode = S_IFREG | mode;
    inode->i_uid  = current_fsuid();
    inode->i_gid  = current_fsgid();

    inode_set_ctime_to_ts(inode, now);
    inode_set_atime_to_ts(inode, now);
    inode_set_mtime_to_ts(inode, now);

    d_add(dentry, inode);

    return 0;
}

static int hodo_unlink(struct inode *dir,struct dentry *dentry) {
    ZONEFS_TRACE();

    struct timespec64 now;
    now = current_time(dir);
    inode_set_ctime_to_ts(dir, now);
    inode_set_atime_to_ts(dir, now);
    inode_set_mtime_to_ts(dir, now);

    //'seq', 'cnv' 디렉토리 속 파일은 삭제되어선 안된다
    const char *target_name = dentry->d_name.name;
    const char *parent_name = dentry->d_parent->d_name.name;
    pr_info("zonefs: unlink parameters, parent name: %s, target name: %s\n", parent_name, target_name); 

    if (!strcmp(parent_name, "seq") || !strcmp(parent_name, "cnv")) {
        pr_info("zonefs: we do not unlink file under the 'seq' or 'cnv'\n");
        return 0;
    }

    //루트 디렉토리는 i_ino와 무관하게 매핑 테이블의 0번째 인덱스에 위치하므로, 수동으로 인덱스를 결정한다
    uint64_t parent_mapping_index;
    uint64_t target_mapping_index;
    if (dir == dentry->d_sb->s_root->d_inode) {
        pr_info("zonefs: unlink in root directory\n"); 
        parent_mapping_index = 0;
        target_mapping_index = dentry->d_inode->i_ino - mapping_info.starting_ino;
    }
    else {
        pr_info("zonefs: unlink in non-root directory\n"); 
        parent_mapping_index = dir->i_ino - mapping_info.starting_ino;
        target_mapping_index = dentry->d_inode->i_ino - mapping_info.starting_ino;
    }

    //매핑 테이블에서 삭제 파일에 관한 행은 이제 쓰이지 않으므로, 비트맵에서 invalid(0)으로 표시한다
    hodo_erase_table_entry(target_mapping_index);

    //target hodo_inode의 i_nlink 수를 0으로 곤치고서 저장장치에 append 하기
    struct hodo_inode target_inode;
    struct hodo_block_pos target_inode_pos;

    target_inode_pos = mapping_info.mapping_table[target_mapping_index];
    hodo_read_struct(target_inode_pos, &target_inode, sizeof(struct hodo_inode));
    
    target_inode.i_nlink = 0;
    
    hodo_write_struct(&target_inode, sizeof(struct hodo_inode), NULL);

    //부모 디렉토리 hodo_inode가 가리키는 직간접적인 데이터블럭에서 삭제 파일의 hodo_dirent를 삭제하고 hodo_inode까지 새로 쓰기
    struct hodo_inode parent_inode;
    struct hodo_block_pos parent_inode_pos;

    parent_inode_pos = mapping_info.mapping_table[parent_mapping_index];
    hodo_read_struct(parent_inode_pos, &parent_inode, sizeof(struct hodo_inode));
    
    struct hodo_block_pos inode_written_pos;
    remove_dirent(&parent_inode, dir, target_name, &inode_written_pos);
    
    mapping_info.mapping_table[parent_mapping_index].zone_id = inode_written_pos.zone_id;
    mapping_info.mapping_table[parent_mapping_index].offset = inode_written_pos.offset;

    //자식 파일이 삭제되었으므로 부모 디렉토리의 VFS 아이노드의 'i_size'을 감소시킨다
    dir->i_size--;

    //VFS 덴트리 캐시 드랍하기
    d_drop(dentry);
    d_add(dentry, NULL);
    return 0;
}

static int hodo_mkdir(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode) {
    ZONEFS_TRACE();

    struct inode *inode;
    struct timespec64 now;
    struct hodo_inode hinode;

    inode = new_inode(dir->i_sb);
    now = current_time(inode);

    // 여기부터 hinode 초기화: 함수로 리팩터링
    hinode.magic[0] = 'I';
    hinode.magic[1] = 'N';
    hinode.magic[2] = 'O';
    hinode.magic[3] = 'D';

    hinode.file_len = 0;

    hinode.name_len = dentry->d_name.len; 
    if (hinode.name_len > HODO_MAX_NAME_LEN) {
        // 구현해야 될 부분: error handling
    }
    memcpy(hinode.name, dentry->d_name.name, hinode.name_len);

    hinode.type = HODO_TYPE_DIR;

    hinode.i_ino = hodo_get_next_ino();

    hinode.i_mode = S_IFDIR | mode; 

    hinode.i_uid = current_fsuid();
    hinode.i_gid = current_fsgid();

    hinode.i_nlink = 1;

    hinode.i_atime = now;
    hinode.i_mtime = now;
    hinode.i_ctime = now;

    // BUG: zone에 걸친 write가 발생할 시, mapping table 관계가 내부에서 변해야 됨
    mapping_info.mapping_table[hinode.i_ino - mapping_info.starting_ino].zone_id = mapping_info.wp.zone_id; 
    mapping_info.mapping_table[hinode.i_ino - mapping_info.starting_ino].offset = mapping_info.wp.offset;
    hodo_write_struct(&hinode, sizeof(struct hodo_inode), NULL);
    // 여기까지 hinode 초기화: 함수로 리팩터링

    add_dirent(dir, &hinode);

    inode->i_ino  = hinode.i_ino;
    inode->i_sb   = dir->i_sb;
    inode->i_op   = &hodo_dir_inode_operations;
    inode->i_fop  = &hodo_dir_operations;
    inode->i_mode = S_IFDIR | mode;
    inode->i_uid  = current_fsuid();
    inode->i_gid  = current_fsgid();

    inode_set_ctime_to_ts(inode, now);
    inode_set_atime_to_ts(inode, now);
    inode_set_mtime_to_ts(inode, now);

    d_add(dentry, inode);

    return 0;
}

static int hodo_rmdir(struct inode *dir, struct dentry *dentry){
    ZONEFS_TRACE();
    
    //seq, cnv 디렉토리는 삭제되면 안된다.
    const char *target_name = dentry->d_name.name;
    const char *parent_name = dentry->d_parent->d_name.name;

    if (!strcmp(target_name, "seq") || !strcmp(target_name, "cnv")) {
        pr_info("zonefs: we do not rmdir 'seq' or 'cnv'\n");
        return 0;
    }

    //예하 파일이 있는 디렉토리는 지우면 안된다.
    if(!check_directory_empty(dentry))
        return -ENOTEMPTY;

    //그 외엔 부모 디렉토리의 nlink수를 줄이고, .unlink(..)를 활용해 rmdir을 수행한다.
    drop_nlink(dir);
    return hodo_unlink(dir, dentry);
}

const struct inode_operations hodo_file_inode_operations = {
    .setattr = hodo_setattr,
};

const struct inode_operations hodo_dir_inode_operations = {
    .lookup     = hodo_lookup,
    .setattr    = hodo_setattr,
    .create     = hodo_create,
    .unlink     = hodo_unlink,
    .mkdir      = hodo_mkdir,
    .rmdir      = hodo_rmdir, 
};

/*-------------------------------------------------------------주소공간 오퍼레이션 함수-------------------------------------------------------------------------------*/
static int hodo_read_folio(struct file *file, struct folio *folio) {
    ZONEFS_TRACE();
    return zonefs_file_aops.read_folio(file, folio);
}

static void hodo_readahead(struct readahead_control *rac) {
    ZONEFS_TRACE();
    zonefs_file_aops.readahead(rac);
}

static int hodo_writepages(struct address_space *mapping,
                                    struct writeback_control *wbc) {
    ZONEFS_TRACE();
    return zonefs_file_aops.writepages(mapping, wbc);
}

static bool hodo_dirty_folio(struct address_space *mapping, struct folio *folio) {
    ZONEFS_TRACE();
    return zonefs_file_aops.dirty_folio(mapping, folio);
}

static bool hodo_release_folio(struct folio *folio, gfp_t gfp) {
    ZONEFS_TRACE();
    return  zonefs_file_aops.release_folio(folio, gfp);
}

static void hodo_invalidate_folio(struct folio *folio, size_t offset, size_t length) {
    ZONEFS_TRACE();
    zonefs_file_aops.invalidate_folio(folio, offset, length);
}

static int hodo_migrate_folio(struct address_space *mapping,
                                        struct folio *dst, struct folio *src, enum migrate_mode mode) {
    ZONEFS_TRACE();
    return zonefs_file_aops.migrate_folio(mapping, dst, src, mode);
}

static bool hodo_is_partially_uptodate(struct folio *folio, size_t from, size_t count) {
    ZONEFS_TRACE();
    return zonefs_file_aops.is_partially_uptodate(folio, from, count);
}

static int hodo_error_remove_folio(struct address_space *mapping, struct folio *folio) {
    ZONEFS_TRACE();
    return zonefs_file_aops.error_remove_folio(mapping, folio);
}

static int hodo_swap_activate(struct swap_info_struct *sis, struct file *file,
                                       sector_t *span) {
    ZONEFS_TRACE();
    return zonefs_file_aops.swap_activate(sis, file, span);
}

const struct address_space_operations hodo_file_aops = {
    .read_folio            = hodo_read_folio,
    .readahead             = hodo_readahead,
    .writepages            = hodo_writepages,
    .dirty_folio           = hodo_dirty_folio,
    .release_folio         = hodo_release_folio,
    .invalidate_folio      = hodo_invalidate_folio,
    .migrate_folio         = hodo_migrate_folio,
    .is_partially_uptodate = hodo_is_partially_uptodate,
    .error_remove_folio    = hodo_error_remove_folio,
    .swap_activate         = hodo_swap_activate,
};

/*-------------------------------------------------------------오퍼레이션 서브 함수-------------------------------------------------------------------------------*/
static struct dentry *hodo_sub_lookup(struct inode* dir, struct dentry* dentry, unsigned int flags) {
    ZONEFS_TRACE();

    const char *name = dentry->d_name.name;
    const char *parent = dentry->d_parent->d_name.name;

    struct timespec64 now;
    now = current_time(dir);
    inode_set_ctime_to_ts(dir, now);
    inode_set_atime_to_ts(dir, now);
    inode_set_mtime_to_ts(dir, now);

    //부모 디렉토리의 hodo 아이노드를 읽어온다
    //루트 노드의 hodo 아이노드 상의 번호는 vfs 아이노드 상의 번호와 달리 0번이니 조작한다 
    uint64_t parent_hodo_inode_number = dir->i_ino;
    struct hodo_block_pos parent_hodo_inode_pos;

    if(dir == dentry->d_sb->s_root->d_inode)
        parent_hodo_inode_pos = mapping_info.mapping_table[0];
    else
        parent_hodo_inode_pos = mapping_info.mapping_table[parent_hodo_inode_number - mapping_info.starting_ino];

    struct hodo_inode parent_hodo_inode;
    hodo_read_struct(parent_hodo_inode_pos, &parent_hodo_inode, sizeof(struct hodo_inode));

    //찾고자 하는 이름을 가진 hodo 아이노드를 읽어온다
    //해당 이름의 아이노드가 저장장치에 없다면, 그냥 없다고 보고하자
    uint64_t target_hodo_inode_number = find_inode_number(&parent_hodo_inode, name);

    if(target_hodo_inode_number == NOTHING_FOUND){
        d_add(dentry, NULL);
        return dentry;
    }

    pr_info("zonefs: target hodo inode number: %d\n", target_hodo_inode_number);
    struct hodo_block_pos target_hodo_inode_pos = mapping_info.mapping_table[target_hodo_inode_number - mapping_info.starting_ino];
    struct hodo_inode target_hodo_inode = { 0, };
    hodo_read_struct(target_hodo_inode_pos, &target_hodo_inode, sizeof(struct hodo_inode));

    //찾던 이름의 hodo 아이노드 정보를 통해 VFS 아이노드를 구성하자
    struct inode *vfs_inode = new_inode(dir->i_sb);
    if (!vfs_inode)
        return ERR_PTR(-ENOMEM);

    vfs_inode->i_ino    = target_hodo_inode.i_ino;
    vfs_inode->i_sb     = dir->i_sb;
    vfs_inode->i_op     = &hodo_dir_inode_operations;
    vfs_inode->i_fop    = &hodo_file_operations;
    vfs_inode->i_mode   = target_hodo_inode.i_mode;
    vfs_inode->i_uid    = target_hodo_inode.i_uid;
    vfs_inode->i_gid    = target_hodo_inode.i_gid;

    inode_set_ctime_to_ts(vfs_inode, target_hodo_inode.i_ctime);
    inode_set_mtime_to_ts(vfs_inode, target_hodo_inode.i_mtime);
    inode_set_atime_to_ts(vfs_inode, target_hodo_inode.i_atime);

    //찾고자 했던 VFS 아이노드를 VFS 덴트리에 이어주자
    d_add(dentry, vfs_inode);
    return dentry;
}

static int hodo_sub_readdir(struct file *file, struct dir_context *ctx) {
    ZONEFS_TRACE();

    struct inode *inode = file_inode(file);
    struct dentry *dentry = file->f_path.dentry;
    const char *name = dentry->d_name.name;

    struct timespec64 now;
    now = current_time(inode);
    inode_set_ctime_to_ts(inode, now);
    inode_set_atime_to_ts(inode, now);
    inode_set_mtime_to_ts(inode, now);

    //(inode->i_size 관리 규정이 확실해지면 기능 활성화하기)
    //ctx->pos는 지금까지 읽은 dirent('.', '..', 'hodo_dirent')의 개수를 나타낸다.
    //total_of_dirent는 hodo 아이노드에 저장된 hodo_dirent의 개수를 나타낸다.
    //책갈피가 책의 마지막장을 넘은 경우는 책을 다 읽은 경우인 것처럼
    //ctx->pos가 total_of_dirent보다 큰 경우는 readdir(..)에서 수행하고 싶은 작업은 완료된 상태이므로 작업을 종료한다.
    // uint64_t total_of_dirent = inode->i_size/sizeof(struct hodo_dirent);
    // if(ctx->pos >= total_of_dirent)
    //     return END_READ;

    //앞으로 '.', '..', 'hodo_dirent'들을 차례로 읽어나가는데, readdir은 언제다 중단되고 재호출 될 수 있기 때문에
    //어느 dirent 까지를 emit했는지를 기록하는 책갈피 역할을 하는 ctx->pos를 참고해,
    //이와 같은 차레(dirent_count)번 째 부터의 dirent부터 emit 해줘야 한다.
    uint64_t dirent_count = 0;

    //ctx->pos가 0~1이면'.'과 '..'을 읽으려고 한다. 
    //둘 다 읽어 ctx->pos가 2로 설정되고 true를 받아 readdir의 다음 단계로 진행한다.
    if (!dir_emit_dots(file, ctx))
        return 0;

    dirent_count = 2;
        
    //루트 디렉터리에서의 ls : 아이노드 번호 0번을 사용
    //그외 사용자 디렉토리에서의 ls : 그냥 주어진 아이노드 번호를 사용
    uint64_t dir_hodo_mapping_index = 0;

    if (inode == d_inode(inode->i_sb->s_root)){
        pr_info("zonefs: readdir on root mount point\n");
        dir_hodo_mapping_index = 0;
    }
    else {
        pr_info("zonefs: readdir on non-root user dir\n");
        dir_hodo_mapping_index = inode->i_ino - mapping_info.starting_ino;
    }

    //디렉토리의 hodo 아이노드를 저장장치로부터 읽어온다
    struct hodo_block_pos dir_hodo_inode_pos = mapping_info.mapping_table[dir_hodo_mapping_index];
    struct hodo_inode dir_hodo_inode = { 0, };
    hodo_read_struct(dir_hodo_inode_pos, &dir_hodo_inode, sizeof(struct hodo_inode));

    //디렉토리 hodo 아이노드가 직간접적으로 가리키는 블럭 안의 덴트리들을 모조리 읽는다
    return read_all_dirents(&dir_hodo_inode, ctx, &dirent_count);
}

static int hodo_sub_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *iattr) {
    ZONEFS_TRACE();

	struct inode *inode = d_inode(dentry);
	int ret;

	if (unlikely(IS_IMMUTABLE(inode)))
		return -EPERM;

	ret = setattr_prepare(&nop_mnt_idmap, dentry, iattr);
    if (ret) {
    	if (d_inode(dentry) == dentry->d_sb->s_root->d_inode) {
	    	pr_info("zonefs: ignoring setattr_prepare failure on root\n");
	    } else {
		    return ret;
	    }
    }


	if (((iattr->ia_valid & ATTR_UID) &&
	     !uid_eq(iattr->ia_uid, inode->i_uid)) ||
	    ((iattr->ia_valid & ATTR_GID) &&
	     !gid_eq(iattr->ia_gid, inode->i_gid))) {
		ret = dquot_transfer(&nop_mnt_idmap, inode, iattr);
		if (ret)
			return ret;
	}

	if (iattr->ia_valid & ATTR_SIZE) {
		ret = zonefs_file_truncate(inode, iattr->ia_size);
		if (ret)
			return ret;
	}

	setattr_copy(&nop_mnt_idmap, inode, iattr);

	return 0;

}


