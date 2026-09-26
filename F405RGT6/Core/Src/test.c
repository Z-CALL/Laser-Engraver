#include "test.h"

void test_laser_PWM_fre_limit(void)
{

    for(int j = 0 ; j < 5; j++){
        for(int i = 0 ; i < 2000 ; i++){
            laser_open();
            BSP_DelayUs((j+1) * 100);
            laser_close();
            BSP_DelayUs((9-j) * 100);
        }
        BSP_DelayMs(200);
    }

    while(1);
}


void test_laser_PWM_fre_limit_12V(void)
{

    for(int j = 0 ; j < 10; j++){
        for(int i = 0 ; i < 200 ; i++){             //1K PWM
            laser_open();
            BSP_DelayUs(500 - 50*j);
            laser_close();
            BSP_DelayUs(500 + 50 * j);
        }
        BSP_DelayMs(200);
    }

    while(1);
}


void test_laser_PWM_fre_limit_24V(void)
{
    for(int j = 0 ; j < 10; j++){
        for(int i = 0 ; i < 200 ; i++){             //1K PWM
            laser_open();
            BSP_DelayUs(300 - 30*j);
            laser_close();
            BSP_DelayUs(700 + 30 * j);
        }
        BSP_DelayMs(200);
    }
    while(1);
}

void test_delay(){
	BSP_DelayMs(10000);
	OLED_ShowNum(0,0,1,OLED_FONT_16,1);
	OLED_Refresh();
	while(1);
}


