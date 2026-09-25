#include "bsp_delay.h"
#include "stdlib.h"

static uint32_t s_cpu_freq_mhz = 168U;
static uint8_t  s_delay_inited = 0U;

alarm_node delay_list = {NULL, 0, 0, 0, NULL, NULL};
clock_alarm clock;
void BSP_DelayInit(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;

    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    s_cpu_freq_mhz = HAL_RCC_GetHCLKFreq() / 1000000U;
    if (s_cpu_freq_mhz == 0U)
    {
        s_cpu_freq_mhz = 168U;
    }

    s_delay_inited = 1U;
}

void BSP_DelayUs(uint32_t us)
{
    uint32_t start_cycle;
    uint32_t need_cycle;

    if (us == 0U)
    {
        return;
    }

    if (s_delay_inited == 0U)
    {
        BSP_DelayInit();
    }

    need_cycle = us * s_cpu_freq_mhz;

    start_cycle = DWT->CYCCNT;

    while ((DWT->CYCCNT - start_cycle) < need_cycle)
    {
        
    }
}

uint8_t* Creat_alarm(uint32_t time){

    alarm_node* prev = &delay_list;
    alarm_node* cur = delay_list.next;
    while(cur != NULL && cur->time <= time){
        prev = cur;
        cur = cur->next;
    }
    alarm_node* b = (alarm_node*)malloc(sizeof(alarm_node));
    if(b == NULL)
        goto error_malloc;
    prev->next = b;
    b->front = prev;
    if(cur != NULL){
        cur->front = b;
        b->next = cur;
    }
    else
    {
        b->next = NULL;
    }
    b->time = time;
    b->DWT_start = DWT->CYCCNT;
    b->DWT_time_ticks = time * s_cpu_freq_mhz;
    b->alarm = (uint8_t*)malloc(sizeof(uint8_t));
    *b->alarm = 0;

    return b->alarm;

error_malloc:
    free(b);
    return NULL;
}

void set_delay_time(uint64_t us_time){
    uint64_t ticks;

    if(clock.start_lock == 0){
        ticks = us_time * (uint64_t)s_cpu_freq_mhz;
        if(ticks > 0xFFFFFFFFULL){
            ticks = 0xFFFFFFFFULL;
        }

        clock.start_time  = DWT->CYCCNT;
        clock.delay_ticks = (uint32_t)ticks;
        clock.alarm       = 0U;
    }
    return;
}

void delay_till(void){
    if (s_delay_inited == 0U)
    {
        BSP_DelayInit();
    }

    if(delay_list.next != NULL){
        if ((DWT->CYCCNT - delay_list.next->DWT_start) >= delay_list.next->DWT_time_ticks)
        {
            *delay_list.next->alarm = 1;
            if(delay_list.next->next != NULL){
                delay_list.next = delay_list.next->next;
                free(delay_list.next->front);
                delay_list.next->front = &delay_list;
            }else{
                free(delay_list.next);
                delay_list.next = NULL;
            }
        }
    }
    return;
}

// void delay_till(void){
//     if (s_delay_inited == 0U)
//     {
//         BSP_DelayInit();
//     }

//     if ((DWT->CYCCNT - clock.start_time) < clock.delay_ticks)
//     {
//         clock.start_lock = 1;
//     }else{
//         clock.start_lock = 0;
//         clock.alarm = 1;
//     }
//     return;
// }

void BSP_DelayMs(uint32_t ms)
{
    if (ms == 0U)
    {
        return;
    }

    HAL_Delay(ms);
}
