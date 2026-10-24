/*
 * trigger.h —— 唤醒触发监听
 */
#ifndef TRIGGER_H
#define TRIGGER_H

#include <stddef.h>

/*
 * 触发源配置
 */
typedef struct {
    const char *uart_dev;      /* 串口设备，如 /dev/ttymxc1；NULL = 不监听 */
    int         uart_baud;     /* 波特率，0 = 115200 */
    const char *uart_keyword;  /* 关键字，如 "WAKE"；NULL = 任何行都触发 */

    int         gpio_num;      /* GPIO 编号（sysfs 号），-1 = 不监听 */
    int         gpio_active;   /* 触发电平：1 = 高电平触发，0 = 低电平触发 */

    unsigned    debounce_ms;   /* 防抖窗口，0 = 3000 */
    unsigned    ask_secs;      /* 触发后录多久，0 = 15 */
    const char *voicectl;      /* voicectl 路径，NULL = /usr/bin/voicectl */

    int         verbose;       /* 打印原始数据 */
} trigger_cfg_t;

/*
 * 启动监听主循环。
 * 阻塞直到收到退出信号（SIGINT/SIGTERM），或发生不可恢复错误。
 * 返回 0 正常退出。
 */
int trigger_run(const trigger_cfg_t *cfg);

/* 请求退出（信号处理里调用） */
void trigger_stop(void);

#endif /* TRIGGER_H */
