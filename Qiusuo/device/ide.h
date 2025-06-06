#ifndef __DEVICE_IDE_H
#define __DEVICE_IDE_H
#include "stdint.h"
#include "list.h"
#include "bitmap.h"
#include "sync.h"

/* 分区结构 */
struct partition {
	uint32_t start_lba;					// 起始扇区
	uint32_t sec_cnt;					// 扇区数
	struct disk* mydisk;				// 分区所属的硬盘
	struct list_elem part_tag;			// 用于队列中的标记
	char name[8];						// 分区名称
	struct super_block* sb;				// 本分区的超级块
	struct bitmap block_bitmap;			// 块位图
	struct bitmap inode_bitmap;			// i结点位图
	struct list open_inodes;			// 本分区打开的i结点队列
};

/* 硬盘结构 */
struct disk {
	char name[8];						// 本硬盘的名称
	struct ide_channel* my_channel;		// 本硬盘属于哪个ide通道
	uint8_t dev_no;						// 本硬盘是主0还是从1
	struct partition prim_parts[4];		// 主分区最多4个
	struct partition logic_parts[8];	// 支持8个逻辑分区
};

/* ata 通道结构 */
struct ide_channel {
	char name[8];						// 本ata名称
	uint16_t port_base;					// 本通道的起始端口号
	uint8_t irq_no;						// 本通道所用的中断号
	struct lock lock;					// 通道锁
	bool expecting_intr;				// 表示等待硬盘的中断
	struct semaphore disk_done;			// 用于阻塞，唤醒驱动程序
	struct disk devices[2];				// 一个通道上连接两个硬盘，一主一从
};

extern uint8_t channel_cnt;				// 按硬盘数计算的通道数
extern struct ide_channel channels[2];	// 有两个ide通道, 硬盘和分区在内存中的信息都在这里
extern struct list partition_list;

void ide_read(struct disk* hd, uint32_t lba, void* buf, uint32_t sec_cnt);
void ide_write(struct disk* hd, uint32_t lba, void* buf, uint32_t sec_cnt);
void intr_hd_handler(uint8_t irq_no);
void ide_init(void);
#endif