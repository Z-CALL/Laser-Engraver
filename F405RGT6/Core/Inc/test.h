#ifndef __TEST_H
#define __TEST_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* ★ 下面这些是测试函数常用的:
     laser.h     -> laser_open() / laser_close()
     bsp_delay.h -> BSP_DelayUs() / BSP_DelayMs()
     oled.h      -> OLED_Init / OLED_ShowString / OLED_FONT_08 / OLED_Refresh
   ★ 为什么放在这里: main.c 也会 include 本头文件, 顺手就能拿到这些声明 ——
     少一处 include 就少一次"implicit declaration"的报错。
     (main.c 里另外单独 include 了 oled.h, 那是给主流程用的, 保留即可) */
#include "laser.h"
#include "bsp_delay.h"
#include "oled.h"

/* Exported functions prototypes ---------------------------------------------*/

/**
  * @brief  激光模块的"时间调制频率上限"测试
  * @note   分段输出 1K / 2.5K / 5K / 10K 的 50% 方波, 看模块从哪个频率开始跟不上。
  *         ⚠ 四段都是 50% 占空 -> 平均功率一样, 所以看"点还分不分得开"(形态),
  *           而不是看深浅。结尾 while(1) 不返回。
  */
void test_laser_PWM_fre_limit(void);
void test_laser_PWM_fre_limit_12V(void);
void test_laser_PWM_fre_limit_24V(void);
void test_delay();

#ifdef __cplusplus
}
#endif

#endif /* __TEST_H */
