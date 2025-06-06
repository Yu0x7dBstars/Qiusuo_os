#include "stdint.h"
#include "dir.h"
#include "ide.h"
#include "super_block.h"
#include "inode.h"
#include "global.h"
#include "stdio-kernel.h"
#include "fs.h"
#include "string.h"
#include "debug.h"
#include "file.h"

struct dir root_dir;

/* 打开根目录 */
void open_root_dir(struct partition* part)
{
	root_dir.inode = inode_open(part, part->sb->root_inode_no);
	root_dir.dir_pos = 0;
}

/* 在分区part上打开i结点为inode_no的目录并返回目录指针 */
struct dir* dir_open(struct partition* part, uint32_t inode_no)
{
	struct dir* pdir = (struct dir*)sys_malloc(sizeof(struct dir));
	pdir->inode = inode_open(part, inode_no);
	pdir->dir_pos = 0;
	return pdir;
}

/* 在 part 分区内的pdir目录内寻找名为name的文件或目录,找到后返回true并将其目录项存入dir_e，否则返回false */
bool search_dir_entry(struct partition* part, struct dir* pdir, char* name, struct dir_entry* dir_e)
{
	uint32_t block_cnt = 140;	// 12个直接块+128个间接块	
	uint32_t* all_blocks = (uint32_t*)sys_malloc(48 + 512);
	if (all_blocks == NULL) {
		printk("search_dir_entry: sys_malloc for all_blocks failed\n");
		return false;
	}

	uint32_t block_idx = 0;
	while (block_idx < 12) {
		all_blocks[block_idx] = pdir->inode->i_sectors[block_idx];
		block_idx++;
	}
	block_idx = 0;

	// 若含有一级间接块表
	if (pdir->inode->i_sectors[12] != 0) {
		// 把一个硬盘里的128个地址写入all_blocks
		// all_blocks 存储的是该文件或目录的所有扇区地址
		ide_read(part->mydisk, pdir->inode->i_sectors[12], all_blocks + 12, 1);
	}

	// 写目录项的时候已保证目录项不跨扇区,这样读目录项时容易处理，只申请容纳1个扇区的内存
	uint8_t* buf = (uint8_t*)sys_malloc(SECTOR_SIZE);
	// p_de 为指向目录项的指针，值为buf起始地址 
	struct dir_entry* p_de = (struct dir_entry*)buf;
	uint32_t dir_entry_size = part->sb->dir_entry_size;

	// 1 扇区内可容纳的目录项个数
	uint32_t dir_entry_cnt = SECTOR_SIZE / dir_entry_size;	

	// 开始在所有块中查找目录项
	while (block_idx < block_cnt) {
		// 块地址为0时表示该块未用，继续在其他块中找
		if (all_blocks[block_idx] == 0) {
			block_idx++;
			continue;
		}
		// 若使用了第block_idx块
		ide_read(part->mydisk, all_blocks[block_idx], buf, 1);
		// 遍历所有目录项
		uint32_t dir_entry_idx = 0;
		while (dir_entry_idx < dir_entry_cnt) {
			if (!strcmp(p_de->filename, name)) {
				memcpy(dir_e, p_de, dir_entry_size);
				sys_free(buf);
				sys_free(all_blocks);
				return true;
			}
			dir_entry_idx++;
			p_de++;
		}
		block_idx++;
		// 此时p_de应该是下一个扇区的基址(指向最后一个目录项后又+1),需要恢复
		p_de = (struct dir_entry*)buf;
		memset(buf, 0, SECTOR_SIZE);
	}
	sys_free(buf);
	sys_free(all_blocks);
	return false;
}

/* 关闭目录 */
void dir_close(struct dir* dir)
{
/*************      根目录不能关闭     *************** 
 * 1 根目录自打开后就不应该关闭，否则还需要再次open_root_dir(); 
 * 2 root_dir 所在的内存是低端1MB之内，并非在堆中，free会出问题 
 * */
	if (dir == &root_dir) {
		return;
	}
	inode_close(dir->inode);
	sys_free(dir);
}

/*在内存中初始化目录项p_de */ 
void create_dir_entry(char* filename, uint32_t inode_no, uint8_t file_type, struct dir_entry* p_de)
{
	ASSERT(strlen(filename) <= MAX_FILENAME_LEN);
	// 初始化目录项
	memcpy(p_de->filename, filename, strlen(filename));
	p_de->i_no = inode_no;
	p_de->f_type = file_type;
}

/* 将目录项p_de写入父目录parent_dir中，io_buf由主调函数提供 */
bool sync_dir_entry(struct dir* parent_dir, struct dir_entry* p_de, void* io_buf)
{
	struct inode* dir_inode = parent_dir->inode;
	uint32_t dir_size = parent_dir->inode->i_size;
	uint32_t dir_entry_size = cur_part->sb->dir_entry_size;
	ASSERT(dir_size % dir_entry_size == 0);					// dir_size应该是dir_entry_size整数倍
	uint32_t dir_entrys_per_sec = 512 / dir_entry_size;		// 每扇区最多的dir_entry数	

	// 将该目录的所有扇区地址（12个直接块+ 128个间接块）存入all_blocks 
	uint8_t block_idx = 0;
	uint32_t all_blocks[140] = {0};
	while (block_idx < 12) {	// 将 12 个直接块存入all_blocks
		all_blocks[block_idx] = parent_dir->inode->i_sectors[block_idx];
		block_idx++;
	}
	
	// 开始遍历所有块以寻找目录项空位，若已有扇区中没有空闲位，在不超过文件大小的情况下申请新扇区来存储新目录项 	
	struct dir_entry* dir_e = (struct dir_entry*)io_buf;	// dir_e用来在io_buf中遍历目录项 
	int32_t block_bitmap_idx = -1;
	int32_t block_lba = -1;
	block_idx = 0;
	
	while (block_idx < 140) {		// 文件（包括目录）最大支持12个直接块+128个间接块＝140个块
		block_bitmap_idx = -1;
		if (all_blocks[block_idx] == 0) {
			block_lba = block_bitmap_alloc(cur_part);
			if (block_lba == -1) {
				printk("alloc block bitmap for sync_dir_entry failed\n");
				return false;
			}
			//  每分配一个块就同步一次block_bitmap
			block_bitmap_idx = block_lba - cur_part->sb->data_start_lba;
			ASSERT(block_bitmap_idx != -1);
			bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);

			block_bitmap_idx = -1;
			if (block_idx < 12) {			// 若是直接块,把申请的块地址直接写入
				dir_inode->i_sectors[block_idx] = block_lba;
				all_blocks[block_idx] = block_lba;
			} else if (block_idx == 12) {	// 若是尚未分配一级间接块表（block_idx等于12表示第0个间接块地址为0）
				dir_inode->i_sectors[block_idx] = block_lba;	// 将上面分配的块作为一级间接块表地址
				
				block_lba = -1;
				block_lba = block_bitmap_alloc(cur_part);
				if (block_lba == -1) {
					block_bitmap_idx = dir_inode->i_sectors[12] - cur_part->sb->data_start_lba;
					bitmap_set(&cur_part->block_bitmap, block_bitmap_idx, 0);
					dir_inode->i_sectors[12] = 0;
					printk("alloc block bitmap for sync_dir_entry failed\n");
					return false;
				}

				// 每分配一个块就同步一次block_bitmap
				block_bitmap_idx = block_lba - cur_part->sb->data_start_lba;
				ASSERT(block_bitmap_idx != -1);
				bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);

				all_blocks[12] = block_lba;		// 把新分配的第0个间接块地址写入一级间接块表
				ide_write(cur_part->mydisk, dir_inode->i_sectors[12], all_blocks + 12, 1);
			} else {	 // 若是间接块未分配
				all_blocks[block_idx] = block_lba;
				ide_write(cur_part->mydisk, dir_inode->i_sectors[12], all_blocks + 12, 1);
			}
			// 再将新目录项p_de写入刚为inode分配的块
			// 因为是新分配的扇区,直接把目录项写在扇区开头就行,因为inode有这个扇区了下次不会来这个分支
			memset(io_buf, 0, SECTOR_SIZE);
			memcpy(io_buf, p_de, dir_entry_size);
			ide_write(cur_part->mydisk, all_blocks[block_idx], io_buf, 1);
			dir_inode->i_size += dir_entry_size;
			return true;
		}
		// 若第 block_idx 块已存在，将其读进内存，然后在该块中查找空目录项
		ide_read(cur_part->mydisk, all_blocks[block_idx], io_buf, 1);
		// 在扇区内查找空目录项 
		uint8_t dir_entry_idx = 0;
		while (dir_entry_idx < dir_entrys_per_sec) {
			//  FT_UNKNOWN 为0，无论是初始化，或是删除文件后,都会将f_type置为FT_UNKNOWN 
			if ((dir_e + dir_entry_idx)->f_type == FT_UNKOWN) {
				memcpy(dir_e + dir_entry_idx, p_de, dir_entry_size);
				ide_write(cur_part->mydisk, all_blocks[block_idx], io_buf, 1);
				dir_inode->i_size += dir_entry_size;
				return true;
			}
			dir_entry_idx++;
		}
		block_idx++;
	}
	printk("directory is full\n");
	return false;
}

/* 把分区part目录pdir中编号为inode_no的目录项删除 */
bool delete_dir_entry(struct partition* part, struct dir* pdir, uint32_t inode_no, void* io_buf)
{
	struct inode* dir_inode = pdir->inode;
	uint32_t block_idx = 0, all_blocks[140] = {0};
	
	/* 收集目录全部块地址 */
	while (block_idx < 12) {
		all_blocks[block_idx] = dir_inode->i_sectors[block_idx];
		block_idx++;
	}
	if (dir_inode->i_sectors[12] != 0) {
		ide_read(part->mydisk, dir_inode->i_sectors[12], all_blocks + 12, 1);
	}
	
	/* 目录项在存储时保证不会跨扇区 */
	uint32_t dir_entry_size = part->sb->dir_entry_size;
	uint32_t dir_entrys_per_sec = SECTOR_SIZE / dir_entry_size;		// 每扇区最大的目录项数目

	struct dir_entry* dir_e = (struct dir_entry*)io_buf;
	struct dir_entry* dir_entry_found = NULL;
	uint8_t dir_entry_idx, dir_entry_cnt;
	bool is_dir_first_block = false;

	/* 遍历所有块，寻找目录项 */
	block_idx = 0;
	while (block_idx < 140) {
		is_dir_first_block = false;
		if (all_blocks[block_idx] == 0) {
			block_idx++;
			continue;
		}

		// 记录每个块的目录项下标和个数
		dir_entry_idx = dir_entry_cnt = 0;
		memset(io_buf, 0, SECTOR_SIZE);
		// 读取扇区,准备遍历这个扇区的目录项
		ide_read(part->mydisk, all_blocks[block_idx], io_buf, 1);
		
		/* 遍历所有的目录项，统计该扇区的目录项数量及是否有待删除的目录项 */
		while (dir_entry_idx < dir_entrys_per_sec) {
			if ((dir_e + dir_entry_idx)->f_type != FT_UNKOWN) {
				if (!strcmp((dir_e + dir_entry_idx)->filename, ".")) {
					is_dir_first_block = true;
				} else if (strcmp((dir_e + dir_entry_idx)->filename, ".") && strcmp((dir_e + dir_entry_idx)->filename, "..")) {
					dir_entry_cnt++;	// 统计此扇区内的目录项个数(不包括.和..)，用来判断删除目录项后是否回收该扇区
					if ((dir_e + dir_entry_idx)->i_no == inode_no) {
						// 如果找到此i结点，就将其记录在dir_entry_found,并且确保目录中只有一个编号为inode_no的inode 
						ASSERT(dir_entry_found == NULL);
						/* 找到后也继续遍历，统计总共的目录项数 */
						dir_entry_found = dir_e + dir_entry_idx;
					}
				}
			}
			dir_entry_idx++;
		}

		/* 若此扇区未找到该目录项，继续在下个扇区中找 */
		if (dir_entry_found == NULL) {
			block_idx++;
			continue;
		}
		
		/* 在此扇区中找到目录项后，清除该目录项并判断是否回收扇区，随后退出循环直接返回 */
		ASSERT(dir_entry_cnt >= 1);
		/* 在当前扇区找到,并且不是第一个扇区，若该扇区上只有该目录项自己，则将整个扇区回收 */
		if (dir_entry_cnt == 1 && !is_dir_first_block) {
			/* a 在块位图中回收该块 */
			// 这里没有像这个if对应的else判断一样将该目录项清空
			uint32_t block_bitmap_idx = all_blocks[block_idx] - part->sb->data_start_lba;
			ASSERT(block_bitmap_idx >= 1);
			bitmap_set(&part->block_bitmap, block_bitmap_idx, 0);
			bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);

			/* b 将块地址从数组i_sectors或索引表中去掉 */
			if (block_idx < 12) {
				dir_inode->i_sectors[block_idx] = 0;
			} else {	// 在一级间接索引表中擦除该间接块地址
				/*先判断一级间接索引表中间接块的数量，如果仅有这1个间接块，连同间接索引表所在的块一同回收 */
				uint32_t indirect_blocks = 0;
				uint32_t indirect_block_idx = 12;
				// 拿到间接块的数量
				while (indirect_block_idx < 140) {
					if (all_blocks[indirect_block_idx] != 0) {
						indirect_blocks++;
					}
					indirect_block_idx++;
				}

				ASSERT(indirect_blocks >= 1);	// 可以有多个间接块
				if (indirect_blocks > 1) {	// 间接索引表中还包括其他间接块，仅在索引表中擦除当前这个间接块地址
					all_blocks[block_idx] = 0;
					ide_write(part->mydisk, dir_inode->i_sectors[12], all_blocks + 12, 1);
				} else {	// 间接索引表中就当前这1个间接块,这个间接块被回收了,把索引表所在的块也回收
					// 书上这里只更新了位图,让那个间接地址块可以被重用,但扇区里面的内容没变,还是一个扇区地址
					// 我们这里把间接块表唯一的间接块地址擦除了并写回
					all_blocks[block_idx] = 0;
					ide_write(part->mydisk, dir_inode->i_sectors[12], all_blocks + 12, 1);

					// 但是间接地址块也只是收回位图，没有擦除硬盘中的间接数据块的数据，虽然被回收后它已经不是间接数据块
					// 应该问题不大，再次被使用时直接覆盖数据就好了，只要管理好新写入的数据就行
					block_bitmap_idx = dir_inode->i_sectors[12] - part->sb->data_start_lba;
					bitmap_set(&part->block_bitmap, block_bitmap_idx, 0);
					bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);
					dir_inode->i_sectors[12] = 0;
					
				}
			}
		} else {	// 仅将该目录项清空
			memset(dir_entry_found, 0, dir_entry_size);
			// 将修改后的iobuf(这个扇区的512字节)写回
			ide_write(part->mydisk, all_blocks[block_idx], io_buf, 1);	
		}

		/* 更新 i结点信息并同步到硬盘 */
		ASSERT(dir_inode->i_size >= dir_entry_size);
		dir_inode->i_size -= dir_entry_size;
		memset(io_buf, 0, SECTOR_SIZE * 2);
		inode_sync(part, dir_inode, io_buf);

		return true;
	}

	/* 所有块中未找到则返回false，出现这种情况应该是serarch_file出错了 */
	return false;
}

/*读取目录，成功返回1个目录项，失败返回NULL */
struct dir_entry* dir_read(struct dir* dir)
{
	struct dir_entry* dir_e = (struct dir_entry*)dir->dir_buf;
	struct inode* dir_inode = dir->inode;
	uint32_t all_blocks[140] = {0}; //, block_cnt = 12;	// 这里的block_cnt没有用上,暂时不知道为啥要它
	uint32_t block_idx = 0, dir_entry_idx = 0;

	// 将目录文件的块地址读入all_blocks
	while (block_idx < 12) {
		all_blocks[block_idx] = dir_inode->i_sectors[block_idx];
		block_idx++;
	}
	if (dir_inode->i_sectors[12] != 0) {
		ide_read(cur_part->mydisk, dir_inode->i_sectors[12], all_blocks + 12, 1);
		// block_cnt = 140;
	}
	block_idx = 0;

	uint32_t cur_dir_entry_pos = 0;	// 当前目录项的偏移，此项用来判断是否是之前已经返回过的目录项
	uint32_t dir_entry_size = cur_part->sb->dir_entry_size;
	uint32_t dir_entrys_per_sec = SECTOR_SIZE / dir_entry_size;

	/* 在目录大小内遍历 */
	while (dir->dir_pos < dir_inode->i_size) {
		if (dir->dir_pos >= dir_inode->i_size) {	// 可读写位置已在文件大小外面,此时已将读完目录项
			return NULL;
		}
		if (all_blocks[block_idx] == 0) {	// 如果此块地址为0，即空块，继续读出下一块
			block_idx++;
			continue;
		}
		memset(dir_e, 0, SECTOR_SIZE);
		ide_read(cur_part->mydisk, all_blocks[block_idx], dir_e, 1);
		
		/* 遍历扇区内所有目录项 */
		dir_entry_idx = 0;
		while (dir_entry_idx < dir_entrys_per_sec) {
			if ((dir_e + dir_entry_idx)->f_type) {	// 如果f_type不等于0，即不等于FT_UNKNOWN
				/* 判断是不是最新的目录项，避免返回曾经已经返回过的目录项 */
				if (cur_dir_entry_pos < dir->dir_pos) {		// 说明这个目录项已经被遍历了
					cur_dir_entry_pos += dir_entry_size;
					dir_entry_idx++;
					continue;
				}
				ASSERT(cur_dir_entry_pos == dir->dir_pos);	// 下一个还未遍历的目录项  	
				dir->dir_pos += dir_entry_size;
				// 更新为新位置，即下一个返回的目录项地址
				return dir_e + dir_entry_idx;
			}
			dir_entry_idx++;
		}
		block_idx++;	// 下一个扇区
	}
	return NULL;
}

/* 判断目录是否为空 */
bool dir_is_empty(struct dir* dir)
{
	struct inode* dir_inode = dir->inode;
	// 若目录只有.和..这两项则为空
	return (dir_inode->i_size == cur_part->sb->dir_entry_size * 2);
}

/* 在父目录parent_dir中删除child_dir, 成功返回0,失败返回-1 */
int32_t dir_remove(struct dir* parent_dir, struct dir* child_dir)
{
	struct inode* child_dir_inode = child_dir->inode;
	/* 空目录只在inode->i_sectors[0]中有扇区，其他扇区都应该为空 */
	int32_t block_idx = 1;
	while (block_idx <= 12) {
		ASSERT(child_dir_inode->i_sectors[block_idx] == 0);
		block_idx++;
	}
	void* io_buf = sys_malloc(SECTOR_SIZE * 2);
	if (io_buf == NULL) {
		printk("in dir_remove: malloc for io_buf failed\n");
		return -1;
	}
	/* 在父目录parent_dir中删除子目录child_dir对应的目录项 */
	delete_dir_entry(cur_part, parent_dir, child_dir_inode->i_no, io_buf);
	
	/* 回收 inode中i_secotrs中所占用的扇区，并同步inode_bitmap和block_bitmap */
	inode_realse(cur_part, child_dir_inode->i_no);
	sys_free(io_buf);
	return 0;
}