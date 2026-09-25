#ifndef __PIN_CONFIGURE_H
#define __PIN_CONFIGURE_H

#define LASER_POWER 4

#define x_dir_grop GPIOA
#define x_dir_pin GPIO_PIN_8
#define x_plus_grop GPIOC
#define x_plus_pin GPIO_PIN_9
#define x_enable_grop GPIOC
#define x_enable_pin GPIO_PIN_8

#define y_dir_grop GPIOB
#define y_dir_pin GPIO_PIN_15
#define y_plus_grop GPIOB
#define y_plus_pin GPIO_PIN_14
#define y_enable_grop GPIOB
#define y_enable_pin GPIO_PIN_13

#define x_limit_r_grop GPIOC
#define x_limit_r_pin GPIO_PIN_0
#define x_limit_r_active 0
#define x_limit_l_grop GPIOC
#define x_limit_l_pin GPIO_PIN_1
#define x_limit_l_active 0

#define y_limit_d_grop GPIOC
#define y_limit_d_pin GPIO_PIN_2
#define y_limit_d_active 1
#define y_limit_u_grop GPIOC
#define y_limit_u_pin GPIO_PIN_3
#define y_limit_u_active 1


#endif
