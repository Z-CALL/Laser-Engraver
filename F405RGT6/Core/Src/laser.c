#include "laser.h"

int laser_open(){
    HAL_GPIO_WritePin(Laser_grop, Laser_pin, GPIO_PIN_SET);
    return 1;
}

int laser_close(){
    HAL_GPIO_WritePin(Laser_grop, Laser_pin, GPIO_PIN_RESET);
    return 1;
}
