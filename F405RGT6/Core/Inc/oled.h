/**
  ******************************************************************************
  * @file    oled.h
  * @brief   SSD1306 OLED 显示屏驱动 (4 针 I2C 接口) —— 头文件
  ******************************************************************************
  * @attention
  *  1. 适用屏：4 针 I2C OLED (VCC / GND / SCL / SDA)，控制芯片 SSD1306
  *     常见规格 0.96 寸 / 1.3 寸，分辨率 128x64。
  *  2. 硬件接线（本工程固定）：
  *         OLED VCC -> 3.3V
  *         OLED GND -> GND
  *         OLED SCL -> PB6  (I2C1_SCL)
  *         OLED SDA -> PB7  (I2C1_SDA)
  *  3. I2C 从机地址：模块背面通常印 0x78 或 0x7A（8 位写法），
  *     对应 7 位地址 0x3C 或 0x3D。本驱动默认 0x3C，
  *     若屏幕无反应，请把 OLED_I2C_ADDR 改成 0x3D 再试。
  *  4. 使用流程：
  *         OLED_Init();                                  // 上电初始化一次
  *         OLED_ShowString(0, 0, "Hello", OLED_FONT_16);  // 直接画到显存
  *         OLED_Refresh();                                // 推送到屏幕
  *     为提高效率，建议先连续调用多个 OLED_ShowXxx 拼好一整屏，
  *     最后统一调用一次 OLED_Refresh()，避免每条都走一次 I2C。
  ******************************************************************************
  */

#ifndef __OLED_H
#define __OLED_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "i2c.h"    /* 提供 hi2c1 的外部声明 */

/* ============================ 硬件参数配置 ============================ */

/** OLED 使用的 I2C 句柄，与 i2c.c 中的 hi2c1 对应 */
#define OLED_I2C_HANDLE         hi2c1

/** OLED 的 7 位 I2C 从机地址（模块常见 0x3C / 0x3D） */
#define OLED_I2C_ADDR           0x3CU

/** 显存宽度，单位像素（固定 128，SSD1306 的 GDDRAM 就是一整行 128 列） */
#define OLED_WIDTH              128U

/** 显存高度，单位像素（0.96/1.3 寸模块均为 64） */
#define OLED_HEIGHT             64U

/** I2C 单次传输的分包大小。HAL 的 Size 参数是 uint16_t 且部分平台限制 <= 255，
    这里按 128 字节一包拆分发送，兼顾效率与兼容性。 */
#define OLED_I2C_CHUNK_SIZE     128U

/** I2C 传输超时时间，单位毫秒 */
#define OLED_I2C_TIMEOUT        100U

/** 屏幕缺省对比度(亮度)，范围 0x00 ~ 0xFF，越大越亮 */
#define OLED_DEFAULT_CONTRAST   0xCFU

/** 字体选项：传入 OLED_ShowXxx 的 font_size 参数 */
#define OLED_FONT_06            6U      /**< 6x8  点阵 ASCII，可打印字符 */
#define OLED_FONT_08            8U      /**< 8x16 点阵 ASCII，可打印字符 */
#define OLED_FONT_16            16U     /**< 16x16 点阵 ASCII，可打印字符 */

/* ============================== 基础接口 ============================== */

/**
  * @brief  初始化 OLED 并点亮
  * @note   内部包含必要的上电延时、SSD1306 寄存器配置、清屏与刷新。
  *         需在 MX_I2C1_Init() 之后调用。
  * @retval HAL_OK 表示初始化过程中 I2C 通信正常
  */
HAL_StatusTypeDef OLED_Init(void);

/**
  * @brief  把显存内容推送到屏幕（真正走 I2C 的一步）
  * @note   所有 OLED_DrawXxx / OLED_ShowXxx / OLED_Clear 只改显存，
  *         必须调用本函数或 OLED_RefreshPage() 才会在屏上显示。
  * @retval HAL_OK 表示发送成功
  */
HAL_StatusTypeDef OLED_Refresh(void);

/**
  * @brief  只刷新指定的一个页（8 行像素），用于局部高速刷新
  * @param  page 页号，范围 0 ~ 7（OLED_HEIGHT / 8 - 1）
  * @retval HAL_OK 表示发送成功
  */
HAL_StatusTypeDef OLED_RefreshPage(uint8_t page);

/**
  * @brief  清空显存（全部填黑），需配合 OLED_Refresh() 才可见
  */
void OLED_Clear(void);

/**
  * @brief  把显存全部填白（点亮所有像素），需配合 OLED_Refresh() 才可见
  */
void OLED_ClearWhite(void);

/**
  * @brief  设置屏幕对比度(亮度)
  * @param  contrast 对比度，0x00 ~ 0xFF，越大越亮
  */
void OLED_SetContrast(uint8_t contrast);

/**
  * @brief  点亮 / 关闭屏幕显示
  * @param  on 1 = 点亮, 0 = 关闭(进入休眠，显存内容保留)
  */
void OLED_DisplayOn(uint8_t on);

/**
  * @brief  反色显示开关
  * @param  invert 1 = 反色, 0 = 正常
  */
void OLED_Invert(uint8_t invert);

/**
  * @brief  画面内容整体上下翻转(仅旋转 180 度的上下部分)
  * @param  flip 1 = 翻转, 0 = 正常
  */
void OLED_FlipVertical(uint8_t flip);

/**
  * @brief  画面内容整体左右翻转
  * @param  flip 1 = 翻转, 0 = 正常
  */
void OLED_FlipHorizontal(uint8_t flip);

/* ============================== 绘图接口 ============================== */

/**
  * @brief  在指定坐标画一个点（写入显存，不立即刷新）
  * @param  x     横坐标，0 ~ OLED_WIDTH-1
  * @param  y     纵坐标，0 ~ OLED_HEIGHT-1
  * @param  value 1 = 点亮该点, 0 = 熄灭该点
  */
void OLED_DrawPoint(uint8_t x, uint8_t y, uint8_t value);

/**
  * @brief  读取显存中某点的值
  * @param  x 横坐标
  * @param  y 纵坐标
  * @retval 1 = 该点已点亮, 0 = 熄灭
  */
uint8_t OLED_GetPoint(uint8_t x, uint8_t y);

/**
  * @brief  画一条线段（Bresenham 算法，支持任意斜率）
  * @param  x0, y0 起点坐标
  * @param  x1, y1 终点坐标
  * @param  value  1 = 画亮线, 0 = 擦除
  */
void OLED_DrawLine(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, uint8_t value);

/**
  * @brief  画一个矩形边框
  * @param  x, y  左上角坐标
  * @param  w, h  宽度与高度（像素）
  * @param  value 1 = 画亮线, 0 = 擦除
  */
void OLED_DrawRect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint8_t value);

/**
  * @brief  画一个实心矩形（常用于做进度条、清除文本背景）
  * @param  x, y  左上角坐标
  * @param  w, h  宽度与高度（像素）
  * @param  value 1 = 填充点亮, 0 = 填充熄灭
  */
void OLED_DrawSolidRect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint8_t value);

/**
  * @brief  画一个圆（中点画圆算法）
  * @param  x0, y0 圆心坐标
  * @param  r      半径（像素）
  * @param  value  1 = 画亮线, 0 = 擦除
  */
void OLED_DrawCircle(uint8_t x0, uint8_t y0, uint8_t r, uint8_t value);

/**
  * @brief  绘制一张位图到指定位置
  * @param  x, y    左上角坐标
  * @param  w, h    位图宽高（像素）
  * @param  p_data  位图数据，按页优先排列（同取模软件"列行式"输出）
  * @param  value   1 = 正常显示, 0 = 反相显示
  * @note   位图取模建议设置：阴码 / 列行式 / 逆向(低位在前)，与 SSD1306 一致。
  */
void OLED_DrawBitmap(uint8_t x, uint8_t y, uint8_t w, uint8_t h,
                     const uint8_t *p_data, uint8_t value);

/* ============================== 字符显示 ============================== */

/**
  * @brief  显示单个 ASCII 字符
  * @param  x, y      左上角坐标
  * @param  ch        要显示的字符，可打印 ASCII (0x20 ~ 0x7E)
  * @param  font_size 字号，取 OLED_FONT_06 / OLED_FONT_08 / OLED_FONT_16
  * @param  value     1 = 正常显示, 0 = 反相显示(黑字白底)
  */
void OLED_ShowChar(uint8_t x, uint8_t y, char ch, uint8_t font_size, uint8_t value);

/**
  * @brief  显示字符串
  * @param  x, y      左上角坐标
  * @param  p_str     以 '\0' 结尾的字符串
  * @param  font_size 字号，取 OLED_FONT_06 / OLED_FONT_08 / OLED_FONT_16
  * @param  value     1 = 正常显示, 0 = 反相显示
  */
void OLED_ShowString(uint8_t x, uint8_t y, const char *p_str,
                     uint8_t font_size, uint8_t value);

/**
  * @brief  显示有符号整型数（十进制）
  * @param  x, y      左上角坐标
  * @param  number    要显示的数字，支持负数（会自动带 '-' 号）
  * @param  font_size 字号
  * @param  value     1 = 正常显示, 0 = 反相显示
  */
void OLED_ShowNum(uint8_t x, uint8_t y, int32_t number,
                  uint8_t font_size, uint8_t value);

/**
  * @brief  显示无符号整型数，并固定位数、前导补零
  * @param  x, y      左上角坐标
  * @param  number    要显示的数字
  * @param  len       固定显示的位数，如 len=4 显示 0015
  * @param  font_size 字号
  * @param  value     1 = 正常显示, 0 = 反相显示
  */
void OLED_ShowUNum(uint8_t x, uint8_t y, uint32_t number, uint8_t len,
                   uint8_t font_size, uint8_t value);

/**
  * @brief  以十六进制显示数值
  * @param  x, y      左上角坐标
  * @param  number    要显示的数值
  * @param  len       固定显示的位数，如 len=4 显示 00FF
  * @param  font_size 字号
  * @param  value     1 = 正常显示, 0 = 反相显示
  */
void OLED_ShowHexNum(uint8_t x, uint8_t y, uint32_t number, uint8_t len,
                     uint8_t font_size, uint8_t value);

/**
  * @brief  以二进制显示数值，常用于查看寄存器位
  * @param  x, y      左上角坐标
  * @param  number    要显示的数值
  * @param  len       固定显示的位数，如 len=8 显示 10110010
  * @param  font_size 字号
  * @param  value     1 = 正常显示, 0 = 反相显示
  */
void OLED_ShowBinNum(uint8_t x, uint8_t y, uint32_t number, uint8_t len,
                     uint8_t font_size, uint8_t value);

/**
  * @brief  显示浮点数（定点方式实现，不依赖 printf 浮点库）
  * @param  x, y      左上角坐标
  * @param  number    要显示的浮点数
  * @param  int_len   整数部分最少显示的位数
  * @param  dec_len   小数部分显示的位数，如 2 表示保留两位
  * @param  font_size 字号
  * @param  value     1 = 正常显示, 0 = 反相显示
  */
void OLED_ShowFloatNum(uint8_t x, uint8_t y, double number, uint8_t int_len,
                       uint8_t dec_len, uint8_t font_size, uint8_t value);

/**
  * @brief  显示中文字符（需自行用取模软件生成字模并填入 oled.c 的字库）
  * @param  x, y      左上角坐标
  * @param  p_str     中文字符串（UTF-8 或 GBK，取决于字库生成方式）
  * @param  font_size 字号，取 OLED_FONT_16
  * @param  value     1 = 正常显示, 0 = 反相显示
  * @note   默认字库为空，添加方法见 oled.c 中 OLED_ShowChinese() 的说明。
  */
void OLED_ShowChinese(uint8_t x, uint8_t y, const char *p_str,
                      uint8_t font_size, uint8_t value);

/* ============================== 屏幕滚动 ============================== */

/**
  * @brief  开启水平左右滚动
  * @param  left      1 = 向左滚动, 0 = 向右滚动
  * @param  start_page 起始页 0 ~ 7
  * @param  end_page   结束页 0 ~ 7
  */
void OLED_ScrollStart(uint8_t left, uint8_t start_page, uint8_t end_page);

/**
  * @brief  停用滚动（恢复正常显示）
  */
void OLED_ScrollStop(void);

#ifdef __cplusplus
}
#endif

#endif /* __OLED_H */
