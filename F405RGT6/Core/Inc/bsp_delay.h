#ifndef __BSP_DELAY_H
#define __BSP_DELAY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

extern uint64_t delay_block;

typedef struct alarm_node{
    struct alarm_node* front;
    uint32_t time;
    uint32_t DWT_start;
    uint32_t DWT_time_ticks;
    uint8_t* alarm;
    struct alarm_node* next;
}alarm_node;

typedef struct clock_alarm {
    uint8_t  start_lock;
    uint32_t start_time;
    uint32_t delay_ticks;
    uint8_t  alarm;
} clock_alarm;

extern clock_alarm clock;

void BSP_DelayInit(void);

void BSP_DelayUs(uint32_t us);

uint8_t* Creat_alarm(uint32_t time);

void BSP_DelayMs(uint32_t ms);

void set_delay_time(uint64_t us_time);

void delay_till(void);

#ifdef __cplusplus
}
#endif

#endif
