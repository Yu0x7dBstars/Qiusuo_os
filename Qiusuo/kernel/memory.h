#ifndef __KERNEL_MEMORY_H
#define __KERNEL_MEMORY_H
#include "stdint.h"
#include "bitmap.h"

/* 虚拟地址池，用于虚拟地址管理 */
struct virtual_addr_pool {
    struct bitmap vaddr_bitmap;     //虚拟地址用到的位图结构
    uint32_t vaddr_start;           //虚拟地址起始地址
};

/* 内存池标记，用于判断用哪个内存池 */
enum pool_flags {
    PF_KERNEL = 1,
    PF_USER = 2
};

#define PG_P_1 1    // 页表项或页目录项存在属性位
#define PG_P_0 0    // 页表项或页目录项存在属性位
#define PG_RW_R 0   // R/W 属性位值，读/执行
#define PG_RW_W 2   // R/W 属性位值，读/写/执行
#define PG_US_S 0   // U/S 属性位值，系统级
#define PG_US_U 4   // U/S 属性位值，用户级

extern struct pool kernel_pool, user_pool;
void mem_init(void);
void* malloc_page(enum pool_flags pf, uint32_t pg_cnt);
void* get_kernel_pages(uint32_t pg_cnt);

#endif