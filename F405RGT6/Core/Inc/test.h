#ifndef __TEST_H
#define __TEST_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "laser.h"
#include "bsp_delay.h"
#include "oled.h"

void test_laser_PWM_fre_limit(void);
void test_laser_PWM_fre_limit_12V(void);
void test_laser_PWM_fre_limit_24V(void);
void test_delay();
void test_homing();
void test_go_to_point();

#ifdef __cplusplus
}
#endif

#endif
