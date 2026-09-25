#include "test.h"

void test_laser_PWM_fre_limit(void)
{
    for(int i = 0 ; i < 400 ; i++){             //1K PWM
        laser_open();
        BSP_DelayUs(500);
        laser_close();
        BSP_DelayUs(500);
    }
    BSP_DelayMs(200);
    for(int i = 0 ; i < 1000 ; i++){               //2.5KPWM
        laser_open();
        BSP_DelayUs(200);
        laser_close();
        BSP_DelayUs(200);
    }
    BSP_DelayMs(200);
    for(int i = 0 ; i < 2000 ; i++){              //5K PWM
        laser_open();
        BSP_DelayUs(100);
        laser_close();
        BSP_DelayUs(100);
    }
    BSP_DelayMs(200);
    for(int i = 0 ; i < 4000 ; i++){                   //10K PWM
        laser_open();
        BSP_DelayUs(50);
        laser_close();
        BSP_DelayUs(50);
    }
    while(1);
}


void test_laser_PWM_fre_limit_12V(void)
{
    for(int i = 0 ; i < 200 ; i++){             //1K PWM
        laser_open();
        BSP_DelayUs(500);
        laser_close();
        BSP_DelayUs(500);
    }
    BSP_DelayMs(200);
    for(int i = 0 ; i < 200 ; i++){             //1K PWM
        laser_open();
        BSP_DelayUs(480);
        laser_close();
        BSP_DelayUs(520);
    }
    BSP_DelayMs(200);
    for(int i = 0 ; i < 200 ; i++){             //1K PWM
        laser_open();
        BSP_DelayUs(460);
        laser_close();
        BSP_DelayUs(540);
    }
    BSP_DelayMs(200);
    for(int i = 0 ; i < 200 ; i++){             //1K PWM
        laser_open();
        BSP_DelayUs(440);
        laser_close();
        BSP_DelayUs(560);
    }
    BSP_DelayMs(200);
    for(int i = 0 ; i < 200 ; i++){             //1K PWM
        laser_open();
        BSP_DelayUs(420);
        laser_close();
        BSP_DelayUs(580);
    }
    BSP_DelayMs(200);
    for(int i = 0 ; i < 200 ; i++){             //1K PWM
        laser_open();
        BSP_DelayUs(400);
        laser_close();
        BSP_DelayUs(600);
    }
    BSP_DelayMs(200);
    for(int i = 0 ; i < 200 ; i++){             //1K PWM
        laser_open();
        BSP_DelayUs(380);
        laser_close();
        BSP_DelayUs(620);
    }
    BSP_DelayMs(200);
    for(int i = 0 ; i < 200 ; i++){             //1K PWM
        laser_open();
        BSP_DelayUs(360);
        laser_close();
        BSP_DelayUs(640);
    }
    BSP_DelayMs(200);
    for(int i = 0 ; i < 200 ; i++){             //1K PWM
        laser_open();
        BSP_DelayUs(340);
        laser_close();
        BSP_DelayUs(660);
    }
    BSP_DelayMs(200);
    for(int i = 0 ; i < 200 ; i++){             //1K PWM
        laser_open();
        BSP_DelayUs(320);
        laser_close();
        BSP_DelayUs(680);
    }
    BSP_DelayMs(200);
    for(int i = 0 ; i < 200 ; i++){             //1K PWM
        laser_open();
        BSP_DelayUs(300);
        laser_close();
        BSP_DelayUs(700);
    }
    BSP_DelayMs(200);
    while(1);
}


void test_laser_PWM_fre_limit_24V(void)
{
    for(int i = 0 ; i < 200 ; i++){             //1K PWM
        laser_open();
        BSP_DelayUs(300);
        laser_close();
        BSP_DelayUs(700);
    }
    BSP_DelayMs(200);
    for(int i = 0 ; i < 200 ; i++){             //1K PWM
        laser_open();
        BSP_DelayUs(275);
        laser_close();
        BSP_DelayUs(725);
    }
    BSP_DelayMs(200);
    for(int i = 0 ; i < 200 ; i++){             //1K PWM
        laser_open();
        BSP_DelayUs(250);
        laser_close();
        BSP_DelayUs(750);
    }
    BSP_DelayMs(200);
    for(int i = 0 ; i < 200 ; i++){             //1K PWM
        laser_open();
        BSP_DelayUs(225);
        laser_close();
        BSP_DelayUs(775);
    }
    BSP_DelayMs(200);
    for(int i = 0 ; i < 200 ; i++){             //1K PWM
        laser_open();
        BSP_DelayUs(200);
        laser_close();
        BSP_DelayUs(800);
    }
    BSP_DelayMs(200);
    for(int i = 0 ; i < 200 ; i++){             //1K PWM
        laser_open();
        BSP_DelayUs(175);
        laser_close();
        BSP_DelayUs(825);
    }
    BSP_DelayMs(200);
    for(int i = 0 ; i < 200 ; i++){             //1K PWM
        laser_open();
        BSP_DelayUs(150);
        laser_close();
        BSP_DelayUs(850);
    }
    BSP_DelayMs(200);
    for(int i = 0 ; i < 200 ; i++){             //1K PWM
        laser_open();
        BSP_DelayUs(125);
        laser_close();
        BSP_DelayUs(875);
    }
    BSP_DelayMs(200);
    for(int i = 0 ; i < 200 ; i++){             //1K PWM
        laser_open();
        BSP_DelayUs(100);
        laser_close();
        BSP_DelayUs(900);
    }
    BSP_DelayMs(200);
    for(int i = 0 ; i < 200 ; i++){             //1K PWM
        laser_open();
        BSP_DelayUs(75);
        laser_close();
        BSP_DelayUs(925);
    }
    BSP_DelayMs(200);
    for(int i = 0 ; i < 200 ; i++){             //1K PWM
        laser_open();
        BSP_DelayUs(50);
        laser_close();
        BSP_DelayUs(950);
    }
    while(1);
}

void test_delay(){
	BSP_DelayMs(10000);
	OLED_ShowNum(0,0,1,OLED_FONT_16,1);
	OLED_Refresh();
	while(1);
}


