#include "init.h"
#include "print.h"
#include "interrupt.h"
#include "../device/timer.h"
#include "memory.h"

/* 初始化所有模块 */
void init_all()
{
    put_str("init_all\n");
    idt_init();
    timer_init();
    mem_init();
}