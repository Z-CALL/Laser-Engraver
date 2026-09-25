/**
  ******************************************************************************
  * @file    oled.c
  * @brief   SSD1306 OLED 显示屏驱动 (4 针 I2C 接口) —— 实现文件
  ******************************************************************************
  * @attention
  *  接线与地址说明详见 oled.h 顶部注释。
  *
  *  SSD1306 显存组织方式（本驱动的一切坐标换算都基于此）：
  *      - 整个 GDDRAM 为 128(列) x 64(行)，按"页"划分为 8 页，每页 8 行像素。
  *      - 每页有 128 个字节，一个字节的 bit0 ~ bit7 对应这一列自上而下的 8 个像素。
  *      - 所以显存数组 OLED_GRAM[8][128] 中：
  *            第一维 = 页号(y / 8)，第二维 = 列号(x)，
  *            字节内的第 (y % 8) 位 = 该像素。
  *      与常见的"逐行扫描"显存不同，务必注意这个"页优先"的结构。
  ******************************************************************************
  */

#include "oled.h"
#include <string.h>

/* Private defines -----------------------------------------------------------*/

/** SSD1306 控制字节：后续跟着的是命令 */
#define OLED_CTRL_CMD           0x00U
/** SSD1306 控制字节：后续跟着的是显存数据 */
#define OLED_CTRL_DATA          0x40U

/** 页总数 = 64 / 8 */
#define OLED_PAGE_COUNT         (OLED_HEIGHT / 8U)

/* Private variables ---------------------------------------------------------*/

/** 显存：OLED_PAGE_COUNT 页 x OLED_WIDTH 列。
    所有绘制操作只改这里，调用 OLED_Refresh() 才会真正发给屏幕。 */
static uint8_t s_oled_gram[OLED_PAGE_COUNT][OLED_WIDTH];

/** 初始化是否成功过，用于兜底自动初始化 */
static uint8_t s_oled_inited = 0U;

/* ====================== 8x16 点阵 ASCII 字库 ======================
   覆盖 ASCII 0x20(空格) ~ 0x7E(~)，共 95 个字符。
   每个字符 16 字节：前 8 字节是上半部分(页 0)，后 8 字节是下半部分(页 1)。
   取模方式：阴码 / 列行式 / 逆向(低位在前)，即每字节 bit0 在最上方。
   字模数据为业界通用的经典 8x16 字库，可直接替换为自己生成的字模。
   ================================================================= */

static const uint8_t s_font_08[95][16] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* ' ' */
    {0x00,0x00,0x00,0xF8,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x33,0x00,0x00,0x00,0x00}, /* '!' */
    {0x00,0x10,0x0C,0x06,0x10,0x0C,0x06,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* '"' */
    {0x40,0xC0,0x78,0x40,0xC0,0x78,0x40,0x00,0x04,0x3F,0x04,0x04,0x3F,0x04,0x04,0x00}, /* '#' */
    {0x00,0x70,0x88,0xFC,0x08,0x30,0x00,0x00,0x00,0x18,0x20,0xFF,0x21,0x1E,0x00,0x00}, /* '$' */
    {0xF0,0x08,0xF0,0x00,0xE0,0x18,0x00,0x00,0x00,0x21,0x1C,0x03,0x1E,0x21,0x1E,0x00}, /* '%' */
    {0x00,0xF0,0x08,0x88,0x70,0x00,0x00,0x00,0x1E,0x21,0x23,0x24,0x19,0x27,0x21,0x10}, /* '&' */
    {0x00,0x12,0x0E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* '\'' */
    {0x00,0x00,0x00,0xE0,0x18,0x04,0x02,0x00,0x00,0x00,0x00,0x07,0x18,0x20,0x40,0x00}, /* '(' */
    {0x00,0x02,0x04,0x18,0xE0,0x00,0x00,0x00,0x00,0x40,0x20,0x18,0x07,0x00,0x00,0x00}, /* ')' */
    {0x40,0x40,0x80,0xF0,0x80,0x40,0x40,0x00,0x02,0x02,0x01,0x0F,0x01,0x02,0x02,0x00}, /* '*' */
    {0x00,0x00,0x00,0xF0,0x00,0x00,0x00,0x00,0x01,0x01,0x01,0x1F,0x01,0x01,0x01,0x00}, /* '+' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xB0,0x70,0x00,0x00,0x00}, /* ',' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x00}, /* '-' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x30,0x30,0x00,0x00,0x00}, /* '.' */
    {0x00,0x00,0x00,0x00,0x80,0x60,0x18,0x04,0x00,0x60,0x18,0x06,0x01,0x00,0x00,0x00}, /* '/' */
    {0x00,0xE0,0x10,0x08,0x08,0x10,0xE0,0x00,0x00,0x0F,0x10,0x20,0x20,0x10,0x0F,0x00}, /* '0' */
    {0x00,0x10,0x10,0xF8,0x00,0x00,0x00,0x00,0x00,0x20,0x20,0x3F,0x20,0x20,0x00,0x00}, /* '1' */
    {0x00,0x70,0x08,0x08,0x08,0x88,0x70,0x00,0x00,0x30,0x28,0x24,0x22,0x21,0x30,0x00}, /* '2' */
    {0x00,0x30,0x08,0x88,0x88,0x48,0x30,0x00,0x00,0x18,0x20,0x20,0x20,0x11,0x0E,0x00}, /* '3' */
    {0x00,0x00,0xC0,0x20,0x10,0xF8,0x00,0x00,0x00,0x07,0x04,0x24,0x24,0x3F,0x24,0x00}, /* '4' */
    {0x00,0xF8,0x08,0x88,0x88,0x08,0x08,0x00,0x00,0x19,0x21,0x20,0x20,0x11,0x0E,0x00}, /* '5' */
    {0x00,0xE0,0x10,0x88,0x88,0x18,0x00,0x00,0x00,0x0F,0x11,0x20,0x20,0x11,0x0E,0x00}, /* '6' */
    {0x00,0x38,0x08,0x08,0xC8,0x38,0x08,0x00,0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, /* '7' */
    {0x00,0x70,0x88,0x08,0x08,0x88,0x70,0x00,0x00,0x1C,0x22,0x21,0x21,0x22,0x1C,0x00}, /* '8' */
    {0x00,0xE0,0x10,0x08,0x08,0x10,0xE0,0x00,0x00,0x00,0x31,0x22,0x22,0x11,0x0F,0x00}, /* '9' */
    {0x00,0x00,0x00,0xC0,0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x30,0x30,0x00,0x00,0x00}, /* ':' */
    {0x00,0x00,0x00,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x60,0xE0,0x00,0x00,0x00}, /* ';' */
    {0x00,0x00,0x80,0x40,0x20,0x10,0x08,0x00,0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x00}, /* '<' */
    {0x00,0x40,0x40,0x40,0x40,0x40,0x40,0x00,0x00,0x02,0x02,0x02,0x02,0x02,0x02,0x00}, /* '=' */
    {0x00,0x08,0x10,0x20,0x40,0x80,0x00,0x00,0x00,0x20,0x10,0x08,0x04,0x02,0x01,0x00}, /* '>' */
    {0x00,0x70,0x48,0x08,0x08,0x08,0xF0,0x00,0x00,0x00,0x00,0x30,0x36,0x01,0x00,0x00}, /* '?' */
    {0xC0,0x30,0xC8,0x28,0xE8,0x10,0xE0,0x00,0x07,0x18,0x27,0x24,0x23,0x14,0x0B,0x00}, /* '@' */
    {0x00,0x00,0xC0,0x38,0xE0,0x00,0x00,0x00,0x20,0x3C,0x23,0x02,0x02,0x27,0x38,0x20}, /* 'A' */
    {0x08,0xF8,0x88,0x88,0x88,0x70,0x00,0x00,0x20,0x3F,0x20,0x20,0x20,0x11,0x0E,0x00}, /* 'B' */
    {0xC0,0x30,0x08,0x08,0x08,0x08,0x38,0x00,0x07,0x18,0x20,0x20,0x20,0x10,0x08,0x00}, /* 'C' */
    {0x08,0xF8,0x08,0x08,0x08,0x10,0xE0,0x00,0x20,0x3F,0x20,0x20,0x20,0x10,0x0F,0x00}, /* 'D' */
    {0x08,0xF8,0x88,0x88,0xE8,0x08,0x10,0x00,0x20,0x3F,0x20,0x20,0x23,0x20,0x18,0x00}, /* 'E' */
    {0x08,0xF8,0x88,0x88,0xE8,0x08,0x10,0x00,0x20,0x3F,0x20,0x00,0x03,0x00,0x00,0x00}, /* 'F' */
    {0xC0,0x30,0x08,0x08,0x08,0x38,0x00,0x00,0x07,0x18,0x20,0x20,0x22,0x1E,0x02,0x00}, /* 'G' */
    {0x08,0xF8,0x08,0x00,0x00,0x08,0xF8,0x08,0x20,0x3F,0x21,0x01,0x01,0x21,0x3F,0x20}, /* 'H' */
    {0x00,0x08,0x08,0xF8,0x08,0x08,0x00,0x00,0x00,0x20,0x20,0x3F,0x20,0x20,0x00,0x00}, /* 'I' */
    {0x00,0x00,0x08,0x08,0xF8,0x08,0x08,0x00,0xC0,0x80,0x80,0x80,0x7F,0x00,0x00,0x00}, /* 'J' */
    {0x08,0xF8,0x88,0xC0,0x28,0x18,0x08,0x00,0x20,0x3F,0x20,0x01,0x26,0x38,0x20,0x00}, /* 'K' */
    {0x08,0xF8,0x08,0x00,0x00,0x00,0x00,0x00,0x20,0x3F,0x20,0x20,0x20,0x20,0x30,0x00}, /* 'L' */
    {0x08,0xF8,0xF8,0x00,0xF8,0xF8,0x08,0x00,0x20,0x3F,0x00,0x3F,0x00,0x3F,0x20,0x00}, /* 'M' */
    {0x08,0xF8,0x30,0xC0,0x00,0x08,0xF8,0x08,0x20,0x3F,0x20,0x00,0x07,0x18,0x3F,0x00}, /* 'N' */
    {0xE0,0x10,0x08,0x08,0x08,0x10,0xE0,0x00,0x0F,0x10,0x20,0x20,0x20,0x10,0x0F,0x00}, /* 'O' */
    {0x08,0xF8,0x08,0x08,0x08,0x08,0xF0,0x00,0x20,0x3F,0x21,0x01,0x01,0x01,0x00,0x00}, /* 'P' */
    {0xE0,0x10,0x08,0x08,0x08,0x10,0xE0,0x00,0x0F,0x18,0x24,0x24,0x38,0x50,0x4F,0x00}, /* 'Q' */
    {0x08,0xF8,0x88,0x88,0x88,0x88,0x70,0x00,0x20,0x3F,0x20,0x00,0x03,0x0C,0x30,0x20}, /* 'R' */
    {0x00,0x70,0x88,0x08,0x08,0x08,0x38,0x00,0x00,0x38,0x20,0x21,0x21,0x22,0x1C,0x00}, /* 'S' */
    {0x18,0x08,0x08,0xF8,0x08,0x08,0x18,0x00,0x00,0x00,0x20,0x3F,0x20,0x00,0x00,0x00}, /* 'T' */
    {0x08,0xF8,0x08,0x00,0x00,0x08,0xF8,0x08,0x00,0x1F,0x20,0x20,0x20,0x20,0x1F,0x00}, /* 'U' */
    {0x08,0x78,0x88,0x00,0x00,0xC8,0x38,0x08,0x00,0x00,0x07,0x38,0x0E,0x01,0x00,0x00}, /* 'V' */
    {0x08,0xF8,0x00,0xF8,0x00,0xF8,0x08,0x00,0x00,0x03,0x3E,0x01,0x3E,0x03,0x00,0x00}, /* 'W' */
    {0x08,0x18,0x68,0x80,0x80,0x68,0x18,0x08,0x20,0x30,0x2C,0x03,0x03,0x2C,0x30,0x20}, /* 'X' */
    {0x08,0x38,0xC8,0x00,0xC8,0x38,0x08,0x00,0x00,0x00,0x20,0x3F,0x20,0x00,0x00,0x00}, /* 'Y' */
    {0x10,0x08,0x08,0x08,0xC8,0x38,0x08,0x00,0x20,0x38,0x26,0x21,0x20,0x20,0x18,0x00}, /* 'Z' */
    {0x00,0x00,0x00,0xFE,0x02,0x02,0x02,0x00,0x00,0x00,0x00,0x7F,0x40,0x40,0x40,0x00}, /* '[' */
    {0x00,0x0C,0x30,0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x06,0x38,0xC0,0x00}, /* '\\' */
    {0x00,0x02,0x02,0x02,0xFE,0x00,0x00,0x00,0x00,0x40,0x40,0x40,0x7F,0x00,0x00,0x00}, /* ']' */
    {0x00,0x00,0x04,0x02,0x02,0x02,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* '^' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80}, /* '_' */
    {0x00,0x02,0x02,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* '`' */
    {0x00,0x00,0x80,0x80,0x80,0x80,0x00,0x00,0x00,0x19,0x24,0x22,0x22,0x22,0x3F,0x20}, /* 'a' */
    {0x08,0xF8,0x00,0x80,0x80,0x00,0x00,0x00,0x00,0x3F,0x11,0x20,0x20,0x11,0x0E,0x00}, /* 'b' */
    {0x00,0x00,0x80,0x80,0x80,0x80,0x00,0x00,0x00,0x0E,0x11,0x20,0x20,0x20,0x11,0x00}, /* 'c' */
    {0x00,0x00,0x80,0x80,0x88,0xF8,0x00,0x00,0x00,0x0E,0x11,0x20,0x20,0x10,0x3F,0x20}, /* 'd' */
    {0x00,0x00,0x80,0x80,0x80,0x80,0x00,0x00,0x00,0x1F,0x22,0x22,0x22,0x22,0x13,0x00}, /* 'e' */
    {0x00,0x80,0x80,0xF0,0x88,0x88,0x88,0x18,0x00,0x20,0x20,0x3F,0x20,0x20,0x00,0x00}, /* 'f' */
    {0x00,0x00,0x80,0x80,0x80,0x80,0x80,0x00,0x00,0x6B,0x94,0x94,0x94,0x94,0x93,0x60}, /* 'g' */
    {0x08,0xF8,0x00,0x80,0x80,0x80,0x00,0x00,0x20,0x3F,0x21,0x00,0x00,0x20,0x3F,0x20}, /* 'h' */
    {0x00,0x80,0x98,0x98,0x00,0x00,0x00,0x00,0x00,0x20,0x20,0x3F,0x20,0x20,0x00,0x00}, /* 'i' */
    {0x00,0x00,0x00,0x80,0x98,0x98,0x00,0x00,0x00,0xC0,0x80,0x80,0x80,0x7F,0x00,0x00}, /* 'j' */
    {0x08,0xF8,0x00,0x00,0x80,0x80,0x80,0x00,0x20,0x3F,0x24,0x02,0x2D,0x30,0x20,0x00}, /* 'k' */
    {0x00,0x08,0x08,0xF8,0x00,0x00,0x00,0x00,0x00,0x20,0x20,0x3F,0x20,0x20,0x00,0x00}, /* 'l' */
    {0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x00,0x20,0x3F,0x20,0x00,0x3F,0x20,0x00,0x3F}, /* 'm' */
    {0x80,0x80,0x00,0x80,0x80,0x80,0x00,0x00,0x20,0x3F,0x21,0x00,0x00,0x20,0x3F,0x20}, /* 'n' */
    {0x00,0x00,0x80,0x80,0x80,0x80,0x00,0x00,0x00,0x1F,0x20,0x20,0x20,0x20,0x1F,0x00}, /* 'o' */
    {0x80,0x80,0x00,0x80,0x80,0x00,0x00,0x00,0x80,0xFF,0xA1,0x20,0x20,0x11,0x0E,0x00}, /* 'p' */
    {0x00,0x00,0x80,0x80,0x80,0x80,0x80,0x00,0x00,0x0E,0x11,0x20,0x20,0xA0,0xFF,0x80}, /* 'q' */
    {0x80,0x80,0x80,0x00,0x80,0x80,0x80,0x00,0x20,0x20,0x3F,0x21,0x20,0x00,0x01,0x00}, /* 'r' */
    {0x00,0x00,0x80,0x80,0x80,0x80,0x80,0x00,0x00,0x33,0x24,0x24,0x24,0x24,0x19,0x00}, /* 's' */
    {0x00,0x80,0x80,0xE0,0x80,0x80,0x00,0x00,0x00,0x00,0x00,0x1F,0x20,0x20,0x00,0x00}, /* 't' */
    {0x80,0x80,0x00,0x00,0x00,0x80,0x80,0x00,0x00,0x1F,0x20,0x20,0x20,0x10,0x3F,0x20}, /* 'u' */
    {0x80,0x80,0x00,0x00,0x00,0x80,0x80,0x00,0x00,0x01,0x0E,0x30,0x08,0x06,0x01,0x00}, /* 'v' */
    {0x80,0x80,0x00,0x80,0x00,0x80,0x80,0x00,0x00,0x0F,0x30,0x0C,0x03,0x0C,0x30,0x0F}, /* 'w' */
    {0x00,0x80,0x80,0x00,0x80,0x80,0x80,0x00,0x00,0x20,0x31,0x2E,0x0E,0x31,0x20,0x00}, /* 'x' */
    {0x80,0x80,0x80,0x00,0x00,0x80,0x80,0x80,0x00,0x81,0x86,0x58,0x38,0x06,0x01,0x00}, /* 'y' */
    {0x00,0x80,0x80,0x80,0x80,0x80,0x80,0x00,0x00,0x21,0x30,0x2C,0x22,0x21,0x30,0x00}, /* 'z' */
    {0x00,0x00,0x00,0x00,0x80,0x7C,0x02,0x02,0x00,0x00,0x00,0x00,0x00,0x3F,0x40,0x40}, /* '{' */
    {0x00,0x00,0x00,0x00,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0x00,0x00}, /* '|' */
    {0x00,0x02,0x02,0x7C,0x80,0x00,0x00,0x00,0x00,0x40,0x40,0x3F,0x00,0x00,0x00,0x00}, /* '}' */
    {0x00,0x06,0x01,0x01,0x02,0x02,0x04,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* '~' */
};

/* Private function prototypes -----------------------------------------------*/

static HAL_StatusTypeDef OLED_WriteCmd(uint8_t cmd);
static HAL_StatusTypeDef OLED_WriteData(const uint8_t *p_data, uint16_t len);
static HAL_StatusTypeDef OLED_SetCursor(uint8_t page, uint8_t col);
static void OLED_ShowFontDot(uint8_t x, uint8_t y, const uint8_t *p_font,
                             uint8_t w, uint8_t h, uint8_t value);
static uint32_t OLED_Pow10(uint8_t n);

/* Exported functions --------------------------------------------------------*/

/* ------------------------------ 底层 I2C ------------------------------ */

/**
  * @brief  向 OLED 发送一条命令
  */
static HAL_StatusTypeDef OLED_WriteCmd(uint8_t cmd)
{
  uint8_t buf[2];

  buf[0] = OLED_CTRL_CMD;   /* 控制字节：0x00 表示后面是命令 */
  buf[1] = cmd;

  return HAL_I2C_Master_Transmit(&OLED_I2C_HANDLE, (uint16_t)(OLED_I2C_ADDR << 1),
                                 buf, 2U, OLED_I2C_TIMEOUT);
}

/**
  * @brief  向 OLED 连续发送显存数据
  * @param  p_data 数据指针
  * @param  len    数据长度
  * @note   HAL 的 Size 参数是 uint16_t，部分平台实现限制单次 <= 255，
  *         这里按 OLED_I2C_CHUNK_SIZE 分多次发送，规避该限制。
  */
static HAL_StatusTypeDef OLED_WriteData(const uint8_t *p_data, uint16_t len)
{
  uint16_t sent = 0U;
  uint16_t chunk;
  uint8_t  buf[OLED_I2C_CHUNK_SIZE + 1U];
  HAL_StatusTypeDef status;

  while (sent < len)
  {
    chunk = (uint16_t)(len - sent);
    if (chunk > OLED_I2C_CHUNK_SIZE)
    {
      chunk = OLED_I2C_CHUNK_SIZE;
    }

    buf[0] = OLED_CTRL_DATA;   /* 控制字节：0x40 表示后面是显存数据 */
    memcpy(&buf[1], &p_data[sent], chunk);

    status = HAL_I2C_Master_Transmit(&OLED_I2C_HANDLE,
                                     (uint16_t)(OLED_I2C_ADDR << 1),
                                     buf, (uint16_t)(chunk + 1U), OLED_I2C_TIMEOUT);
    if (status != HAL_OK)
    {
      return status;
    }

    sent = (uint16_t)(sent + chunk);
  }

  return HAL_OK;
}

/**
  * @brief  设置 SSD1306 的写入光标（页地址 + 列地址）
  * @param  page 页号 0 ~ 7
  * @param  col  列号 0 ~ 127
  */
static HAL_StatusTypeDef OLED_SetCursor(uint8_t page, uint8_t col)
{
  if (OLED_WriteCmd((uint8_t)(0xB0U | (page & 0x07U))) != HAL_OK)   /* 页地址 */
  {
    return HAL_ERROR;
  }
  if (OLED_WriteCmd((uint8_t)(0x00U | (col & 0x0FU))) != HAL_OK)    /* 列地址低 4 位 */
  {
    return HAL_ERROR;
  }
  if (OLED_WriteCmd((uint8_t)(0x10U | ((col >> 4) & 0x0FU))) != HAL_OK) /* 列地址高 4 位 */
  {
    return HAL_ERROR;
  }

  return HAL_OK;
}

/* ------------------------------ 初始化 ------------------------------ */

HAL_StatusTypeDef OLED_Init(void)
{
  uint8_t i;

  /* 先清空显存，避免上电瞬间出现雪花 */
  OLED_Clear();

  HAL_Delay(100);   /* 等待 OLED 内部电源稳定，SSD1306 数据手册要求 */

  /* ---- SSD1306 标准初始化命令序列 ---- */
  OLED_WriteCmd(0xAE);   /* 关闭显示 */
  OLED_WriteCmd(0x20);   /* 设置内存寻址模式 */
  OLED_WriteCmd(0x10);   /* 页寻址模式（后续靠 OLED_SetCursor 定位） */
  OLED_WriteCmd(0xB0);   /* 页起始地址 = 0 */
  OLED_WriteCmd(0xC8);   /* 扫描方向：从下到上（屏幕上下方向） */
  OLED_WriteCmd(0x00);   /* 低列地址 = 0 */
  OLED_WriteCmd(0x10);   /* 高列地址 = 0 */
  OLED_WriteCmd(0x40);   /* 显示起始行 = 0 */
  OLED_WriteCmd(0x81);   /* 设置对比度 */
  OLED_WriteCmd(OLED_DEFAULT_CONTRAST);
  OLED_WriteCmd(0xA1);   /* 段重映射：左右方向（0xA1 正常方向） */
  OLED_WriteCmd(0xA6);   /* 正常显示（非反色） */
  OLED_WriteCmd(0xA8);   /* 设置多路复用比 = 64 */
  OLED_WriteCmd(0x3F);
  OLED_WriteCmd(0xA4);   /* 输出跟随显存内容（非全亮测试） */
  OLED_WriteCmd(0xD3);   /* 设置显示偏移 = 0 */
  OLED_WriteCmd(0x00);
  OLED_WriteCmd(0xD5);   /* 设置时钟分频 */
  OLED_WriteCmd(0xF0);
  OLED_WriteCmd(0xD9);   /* 设置预充电周期 */
  OLED_WriteCmd(0x22);
  OLED_WriteCmd(0xDA);   /* 设置 COM 引脚硬件配置 */
  OLED_WriteCmd(0x12);
  OLED_WriteCmd(0xDB);   /* 设置 VCOMH 电压 */
  OLED_WriteCmd(0x20);
  OLED_WriteCmd(0x8D);   /* 电荷泵设置 */
  OLED_WriteCmd(0x14);   /* 使能电荷泵（OLED 全靠它提供高压，必须开） */
  OLED_WriteCmd(0xAF);   /* 打开显示 */

  /* 把清空后的显存推送上去，确保开机就是干净的黑屏 */
  for (i = 0U; i < OLED_PAGE_COUNT; i++)
  {
    if (OLED_RefreshPage(i) != HAL_OK)
    {
      return HAL_ERROR;
    }
  }

  s_oled_inited = 1U;

  return HAL_OK;
}

/* ------------------------------ 刷新 ------------------------------ */

HAL_StatusTypeDef OLED_RefreshPage(uint8_t page)
{
  HAL_StatusTypeDef status;

  if (page >= OLED_PAGE_COUNT)
  {
    return HAL_ERROR;
  }

  /* 光标定位到该页第 0 列，然后整页 128 字节连续写入 */
  status = OLED_SetCursor(page, 0U);
  if (status != HAL_OK)
  {
    return status;
  }

  return OLED_WriteData(&s_oled_gram[page][0], OLED_WIDTH);
}

HAL_StatusTypeDef OLED_Refresh(void)
{
  uint8_t page;
  HAL_StatusTypeDef status;

  /* 兜底：忘记调用 OLED_Init() 时自动补一次 */
  if (s_oled_inited == 0U)
  {
    if (OLED_Init() != HAL_OK)
    {
      return HAL_ERROR;
    }
  }

  for (page = 0U; page < OLED_PAGE_COUNT; page++)
  {
    status = OLED_RefreshPage(page);
    if (status != HAL_OK)
    {
      return status;
    }
  }

  return HAL_OK;
}

/* ------------------------------ 显存操作 ------------------------------ */

void OLED_Clear(void)
{
  memset(s_oled_gram, 0x00, sizeof(s_oled_gram));
}

void OLED_ClearWhite(void)
{
  memset(s_oled_gram, 0xFF, sizeof(s_oled_gram));
}

/* ------------------------------ 显示控制 ------------------------------ */

void OLED_SetContrast(uint8_t contrast)
{
  OLED_WriteCmd(0x81);
  OLED_WriteCmd(contrast);
}

void OLED_DisplayOn(uint8_t on)
{
  OLED_WriteCmd(on ? 0xAFU : 0xAEU);
}

void OLED_Invert(uint8_t invert)
{
  OLED_WriteCmd(invert ? 0xA7U : 0xA6U);
}

void OLED_FlipVertical(uint8_t flip)
{
  OLED_WriteCmd(flip ? 0xC0U : 0xC8U);
}

void OLED_FlipHorizontal(uint8_t flip)
{
  OLED_WriteCmd(flip ? 0xA0U : 0xA1U);
}

/* ------------------------------ 绘图 ------------------------------ */

void OLED_DrawPoint(uint8_t x, uint8_t y, uint8_t value)
{
  uint8_t page;
  uint8_t bit;

  if ((x >= OLED_WIDTH) || (y >= OLED_HEIGHT))
  {
    return;   /* 越界直接忽略，避免踩坏别的像素 */
  }

  page = (uint8_t)(y >> 3);        /* y / 8 得到页号 */
  bit  = (uint8_t)(y & 0x07U);     /* y % 8 得到页内偏移 */

  if (value)
  {
    s_oled_gram[page][x] |= (uint8_t)(1U << bit);
  }
  else
  {
    s_oled_gram[page][x] &= (uint8_t)(~(1U << bit));
  }
}

uint8_t OLED_GetPoint(uint8_t x, uint8_t y)
{
  uint8_t page;
  uint8_t bit;

  if ((x >= OLED_WIDTH) || (y >= OLED_HEIGHT))
  {
    return 0U;
  }

  page = (uint8_t)(y >> 3);
  bit  = (uint8_t)(y & 0x07U);

  return (uint8_t)((s_oled_gram[page][x] >> bit) & 0x01U);
}

void OLED_DrawLine(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, uint8_t value)
{
  int16_t dx = (int16_t)x1 - (int16_t)x0;
  int16_t dy = (int16_t)y1 - (int16_t)y0;
  int16_t sx = (dx >= 0) ? 1 : -1;
  int16_t sy = (dy >= 0) ? 1 : -1;
  int16_t err;
  int16_t e2;
  int16_t x = (int16_t)x0;
  int16_t y = (int16_t)y0;

  if (dx < 0)
  {
    dx = -dx;
  }
  if (dy < 0)
  {
    dy = -dy;
  }

  /* Bresenham 直线算法，误差项在 x/y 之间递推，无需浮点运算 */
  err = (int16_t)(dx - dy);

  while (1)
  {
    OLED_DrawPoint((uint8_t)x, (uint8_t)y, value);

    if ((x == (int16_t)x1) && (y == (int16_t)y1))
    {
      break;
    }

    e2 = (int16_t)(err << 1);
    if (e2 > -dy)
    {
      err = (int16_t)(err - dy);
      x = (int16_t)(x + sx);
    }
    if (e2 < dx)
    {
      err = (int16_t)(err + dx);
      y = (int16_t)(y + sy);
    }
  }
}

void OLED_DrawRect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint8_t value)
{
  if ((w == 0U) || (h == 0U))
  {
    return;
  }

  OLED_DrawLine(x, y, (uint8_t)(x + w - 1U), y, value);                       /* 上边 */
  OLED_DrawLine(x, (uint8_t)(y + h - 1U), (uint8_t)(x + w - 1U),
                (uint8_t)(y + h - 1U), value);                                /* 下边 */
  OLED_DrawLine(x, y, x, (uint8_t)(y + h - 1U), value);                       /* 左边 */
  OLED_DrawLine((uint8_t)(x + w - 1U), y, (uint8_t)(x + w - 1U),
                (uint8_t)(y + h - 1U), value);                                /* 右边 */
}

void OLED_DrawSolidRect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint8_t value)
{
  uint8_t i;
  uint8_t j;

  if ((w == 0U) || (h == 0U))
  {
    return;
  }

  for (i = 0U; i < h; i++)
  {
    for (j = 0U; j < w; j++)
    {
      OLED_DrawPoint((uint8_t)(x + j), (uint8_t)(y + i), value);
    }
  }
}

void OLED_DrawCircle(uint8_t x0, uint8_t y0, uint8_t r, uint8_t value)
{
  int16_t x = 0;
  int16_t y = (int16_t)r;
  int16_t d = 3 - (2 * (int16_t)r);   /* 中点画圆算法的判别式初值 */

  if (r == 0U)
  {
    OLED_DrawPoint(x0, y0, value);
    return;
  }

  while (x <= y)
  {
    /* 利用八分对称性，一次算出圆周上的 8 个点 */
    OLED_DrawPoint((uint8_t)(x0 + x), (uint8_t)(y0 + y), value);
    OLED_DrawPoint((uint8_t)(x0 - x), (uint8_t)(y0 + y), value);
    OLED_DrawPoint((uint8_t)(x0 + x), (uint8_t)(y0 - y), value);
    OLED_DrawPoint((uint8_t)(x0 - x), (uint8_t)(y0 - y), value);
    OLED_DrawPoint((uint8_t)(x0 + y), (uint8_t)(y0 + x), value);
    OLED_DrawPoint((uint8_t)(x0 - y), (uint8_t)(y0 + x), value);
    OLED_DrawPoint((uint8_t)(x0 + y), (uint8_t)(y0 - x), value);
    OLED_DrawPoint((uint8_t)(x0 - y), (uint8_t)(y0 - x), value);

    if (d < 0)
    {
      d = (int16_t)(d + 4 * x + 6);
    }
    else
    {
      d = (int16_t)(d + 4 * (x - y) + 10);
      y--;
    }
    x++;
  }
}

void OLED_DrawBitmap(uint8_t x, uint8_t y, uint8_t w, uint8_t h,
                     const uint8_t *p_data, uint8_t value)
{
  uint8_t page_count;
  uint8_t page;
  uint8_t col;
  uint8_t bit;
  uint8_t byte;
  uint8_t dot;

  if ((p_data == NULL) || (w == 0U) || (h == 0U))
  {
    return;
  }

  page_count = (uint8_t)((h + 7U) / 8U);   /* 位图跨了多少页 */

  for (page = 0U; page < page_count; page++)
  {
    for (col = 0U; col < w; col++)
    {
      byte = p_data[page * w + col];   /* 列行式取模：同一页内按列连续存放 */

      for (bit = 0U; bit < 8U; bit++)
      {
        if ((byte & (uint8_t)(1U << bit)) != 0U)
        {
          dot = value ? 1U : 0U;
        }
        else
        {
          dot = value ? 0U : 1U;
        }
        OLED_DrawPoint((uint8_t)(x + col), (uint8_t)(y + page * 8U + bit), dot);
      }
    }
  }
}

/* ------------------------------ 字符显示 ------------------------------ */

/**
  * @brief  把一行字模点阵逐列写入显存
  * @param  x, y    左上角坐标
  * @param  p_font  字模数据首地址
  * @param  w, h    字符宽高（像素）
  * @param  value   1 = 正常显示, 0 = 反相显示
  * @note   字模按"列行式"存放：每列 h/8 个字节，低字节对应靠上的页。
  */
static void OLED_ShowFontDot(uint8_t x, uint8_t y, const uint8_t *p_font,
                             uint8_t w, uint8_t h, uint8_t value)
{
  uint8_t col;
  uint8_t page;
  uint8_t bit;
  uint8_t byte;
  uint8_t dot;
  uint8_t page_count;

  if (p_font == NULL)
  {
    return;
  }

  page_count = (uint8_t)(h / 8U);

  for (col = 0U; col < w; col++)
  {
    for (page = 0U; page < page_count; page++)
    {
      byte = p_font[col * page_count + page];

      for (bit = 0U; bit < 8U; bit++)
      {
        if ((byte & (uint8_t)(1U << bit)) != 0U)
        {
          dot = value ? 1U : 0U;
        }
        else
        {
          dot = value ? 0U : 1U;
        }
        OLED_DrawPoint((uint8_t)(x + col),
                       (uint8_t)(y + page * 8U + bit), dot);
      }
    }
  }
}

/**
  * @brief  取 8x16 字库中某个字符的两页数据（上半页 + 下半页）
  * @param  ch 待查字符
  * @retval 指向 16 字节字模的指针；非可打印字符返回空格的字模
  */
static const uint8_t *OLED_GetFont08(char ch)
{
  uint8_t idx;

  if ((ch < 0x20) || (ch > 0x7E))
  {
    ch = ' ';   /* 不可打印字符统一显示为空格 */
  }

  idx = (uint8_t)(ch - 0x20);

  return s_font_08[idx];
}

void OLED_ShowChar(uint8_t x, uint8_t y, char ch, uint8_t font_size, uint8_t value)
{
  const uint8_t *p_font;
  uint8_t upper[8];
  uint8_t lower[8];
  uint8_t i;

  switch (font_size)
  {
    case OLED_FONT_08:
      /* 8x16 字库：16 字节 = 前 8 字节上半页 + 后 8 字节下半页 */
      p_font = OLED_GetFont08(ch);

      for (i = 0U; i < 8U; i++)
      {
        upper[i] = p_font[i];
        lower[i] = p_font[i + 8U];
      }

      OLED_ShowFontDot(x, y, upper, 8U, 8U, value);       /* 上半页 */
      OLED_ShowFontDot(x, (uint8_t)(y + 8U), lower, 8U, 8U, value); /* 下半页 */
      break;

    case OLED_FONT_06:
      /* 6x8 字库：直接复用 8x16 字库的上半页，右边 2 列留空 */
      p_font = OLED_GetFont08(ch);
      OLED_ShowFontDot(x, y, p_font, 8U, 8U, value);
      break;

    case OLED_FONT_16:
      /* 16x16 字库需要自行取模并添加到 oled.c，默认退回 8x16 显示 */
      p_font = OLED_GetFont08(ch);

      for (i = 0U; i < 8U; i++)
      {
        upper[i] = p_font[i];
        lower[i] = p_font[i + 8U];
      }

      OLED_ShowFontDot(x, y, upper, 8U, 8U, value);
      OLED_ShowFontDot(x, (uint8_t)(y + 8U), lower, 8U, 8U, value);
      break;

    default:
      /* 未知字号也走 8x16，避免死机 */
      p_font = OLED_GetFont08(ch);

      for (i = 0U; i < 8U; i++)
      {
        upper[i] = p_font[i];
        lower[i] = p_font[i + 8U];
      }

      OLED_ShowFontDot(x, y, upper, 8U, 8U, value);
      OLED_ShowFontDot(x, (uint8_t)(y + 8U), lower, 8U, 8U, value);
      break;
  }
}

/**
  * @brief  取得某个字号的字符宽度（像素），供 OLED_ShowString 换行使用
  */
static uint8_t OLED_GetCharWidth(uint8_t font_size)
{
  switch (font_size)
  {
    case OLED_FONT_06: return 6U;
    case OLED_FONT_16: return 16U;
    case OLED_FONT_08:
    default:           return 8U;
  }
}

/**
  * @brief  取得某个字号的字符高度（像素）
  */
static uint8_t OLED_GetCharHeight(uint8_t font_size)
{
  switch (font_size)
  {
    case OLED_FONT_06: return 8U;
    case OLED_FONT_16: return 16U;
    case OLED_FONT_08:
    default:           return 16U;
  }
}

void OLED_ShowString(uint8_t x, uint8_t y, const char *p_str,
                     uint8_t font_size, uint8_t value)
{
  uint8_t cx = x;
  uint8_t cy = y;
  uint8_t char_w;
  uint8_t char_h;

  if (p_str == NULL)
  {
    return;
  }

  char_w = OLED_GetCharWidth(font_size);
  char_h = OLED_GetCharHeight(font_size);

  while (*p_str != '\0')
  {
    /* 换行符：回到起始 x，向下走一行 */
    if (*p_str == '\n')
    {
      cx = x;
      cy = (uint8_t)(cy + char_h);
      p_str++;
      continue;
    }

    /* 超出屏幕宽度自动折行 */
    if ((uint16_t)cx + char_w > OLED_WIDTH)
    {
      cx = x;
      cy = (uint8_t)(cy + char_h);
    }

    /* 超出屏幕高度则停止，防止越界写显存 */
    if ((uint16_t)cy + char_h > OLED_HEIGHT)
    {
      break;
    }

    OLED_ShowChar(cx, cy, *p_str, font_size, value);

    cx = (uint8_t)(cx + char_w);
    p_str++;
  }
}

/**
  * @brief  求 10 的 n 次方（定点显示数字时用来逐位取数）
  */
static uint32_t OLED_Pow10(uint8_t n)
{
  uint32_t result = 1U;

  while (n > 0U)
  {
    result *= 10U;
    n--;
  }

  return result;
}

void OLED_ShowNum(uint8_t x, uint8_t y, int32_t number,
                  uint8_t font_size, uint8_t value)
{
  uint8_t buf[12];
  uint8_t len = 0U;
  uint8_t i;
  uint32_t abs_val;
  uint8_t char_w;
  uint8_t cx = x;

  char_w = OLED_GetCharWidth(font_size);

  if (number < 0)
  {
    /* 单独画一个负号，再把剩余部分按正数处理 */
    OLED_ShowChar(cx, y, '-', font_size, value);
    cx = (uint8_t)(cx + char_w);

    /* 用 uint32_t 承载绝对值，避免 INT32_MIN 取反溢出 */
    abs_val = (uint32_t)(-(int64_t)number);
  }
  else
  {
    abs_val = (uint32_t)number;
  }

  /* 先拆成十进制各位，存入缓冲区（此时是逆序的） */
  do
  {
    buf[len] = (uint8_t)('0' + (abs_val % 10U));
    abs_val /= 10U;
    len++;
  } while ((abs_val > 0U) && (len < sizeof(buf)));

  /* 再逆序输出，得到正确的数位顺序（符号已单独处理，缓冲区里只有数字） */
  for (i = 0U; i < len; i++)
  {
    OLED_ShowChar(cx, y, (char)buf[len - 1U - i], font_size, value);
    cx = (uint8_t)(cx + char_w);
  }
}

void OLED_ShowUNum(uint8_t x, uint8_t y, uint32_t number, uint8_t len,
                   uint8_t font_size, uint8_t value)
{
  uint8_t i;
  uint8_t cx = x;
  uint8_t char_w = OLED_GetCharWidth(font_size);
  uint32_t div;
  uint8_t digit;

  if (len == 0U)
  {
    return;
  }
  if (len > 10U)
  {
    len = 10U;   /* uint32 最多 10 位十进制 */
  }

  /* 从最高位开始逐位取出并显示，不足位数补 '0' */
  div = OLED_Pow10((uint8_t)(len - 1U));

  for (i = 0U; i < len; i++)
  {
    digit = (uint8_t)((number / div) % 10U);
    OLED_ShowChar(cx, y, (char)('0' + digit), font_size, value);
    cx = (uint8_t)(cx + char_w);

    if (div > 1U)
    {
      div /= 10U;
    }
  }
}

void OLED_ShowHexNum(uint8_t x, uint8_t y, uint32_t number, uint8_t len,
                     uint8_t font_size, uint8_t value)
{
  uint8_t i;
  uint8_t cx = x;
  uint8_t char_w = OLED_GetCharWidth(font_size);
  uint8_t nibble;
  uint8_t shift;

  if ((len == 0U) || (len > 8U))
  {
    return;
  }

  /* 从最高 4 位开始取，逐位输出；10~15 输出 A~F */
  for (i = 0U; i < len; i++)
  {
    shift = (uint8_t)((len - 1U - i) * 4U);
    nibble = (uint8_t)((number >> shift) & 0x0FU);

    if (nibble < 10U)
    {
      OLED_ShowChar(cx, y, (char)('0' + nibble), font_size, value);
    }
    else
    {
      OLED_ShowChar(cx, y, (char)('A' + (nibble - 10U)), font_size, value);
    }

    cx = (uint8_t)(cx + char_w);
  }
}

void OLED_ShowBinNum(uint8_t x, uint8_t y, uint32_t number, uint8_t len,
                     uint8_t font_size, uint8_t value)
{
  uint8_t i;
  uint8_t cx = x;
  uint8_t char_w = OLED_GetCharWidth(font_size);
  uint8_t bit;

  if ((len == 0U) || (len > 32U))
  {
    return;
  }

  /* 最高位在左，与手写二进制一致 */
  for (i = 0U; i < len; i++)
  {
    bit = (uint8_t)((number >> (len - 1U - i)) & 0x01U);
    OLED_ShowChar(cx, y, (char)('0' + bit), font_size, value);
    cx = (uint8_t)(cx + char_w);
  }
}

void OLED_ShowFloatNum(uint8_t x, uint8_t y, double number, uint8_t int_len,
                       uint8_t dec_len, uint8_t font_size, uint8_t value)
{
  uint8_t cx = x;
  uint8_t char_w = OLED_GetCharWidth(font_size);
  uint8_t i;
  double abs_val;
  uint32_t int_part;
  uint32_t scale;
  uint32_t dec_part;
  uint8_t out_len;
  uint32_t div;

  if (number < 0.0)
  {
    OLED_ShowChar(cx, y, '-', font_size, value);
    cx = (uint8_t)(cx + char_w);
    abs_val = -number;
  }
  else
  {
    abs_val = number;
  }

  /* 整数部分：按 int_len 位补零输出 */
  if (int_len == 0U)
  {
    int_len = 1U;
  }
  if (int_len > 10U)
  {
    int_len = 10U;
  }

  /* 先四舍五入到指定小数位，避免出现 1.29 显示成 1.2 的截断问题 */
  if (dec_len > 0U)
  {
    double round_base = 0.5;
    for (i = 0U; i < dec_len; i++)
    {
      round_base /= 10.0;
    }
    abs_val += round_base;
  }

  int_part = (uint32_t)abs_val;

  /* 进位保护：四舍五入可能让整数部分多出一位（如 99.999 保留 2 位小数 -> 100.00）。
     此时自动把整数位数放宽到实际需要的位数，保证显示的是真值而不是被截断的错值。 */
  while ((int_len < 10U) && (int_part >= OLED_Pow10(int_len)))
  {
    int_len++;
  }

  div = OLED_Pow10((uint8_t)(int_len - 1U));
  for (i = 0U; i < int_len; i++)
  {
    OLED_ShowChar(cx, y, (char)('0' + (uint8_t)((int_part / div) % 10U)),
                  font_size, value);
    cx = (uint8_t)(cx + char_w);
    if (div > 1U)
    {
      div /= 10U;
    }
  }

  if (dec_len == 0U)
  {
    return;
  }
  if (dec_len > 9U)
  {
    dec_len = 9U;
  }

  OLED_ShowChar(cx, y, '.', font_size, value);
  cx = (uint8_t)(cx + char_w);

  /* 小数部分：把小数位整体提成整数，再按位输出 */
  scale = OLED_Pow10(dec_len);
  dec_part = (uint32_t)((abs_val - (double)int_part) * (double)scale);
  out_len = dec_len;

  div = OLED_Pow10((uint8_t)(out_len - 1U));
  for (i = 0U; i < out_len; i++)
  {
    OLED_ShowChar(cx, y, (char)('0' + (uint8_t)((dec_part / div) % 10U)),
                  font_size, value);
    cx = (uint8_t)(cx + char_w);
    if (div > 1U)
    {
      div /= 10U;
    }
  }
}

void OLED_ShowChinese(uint8_t x, uint8_t y, const char *p_str,
                      uint8_t font_size, uint8_t value)
{
  /* ========================================================================
     中文字模需要自行生成，步骤如下：
       1) 用 PCtoLCD2002 或类似取模软件，设置：
              阴码 / 列行式 / 逆向(低位在前) / 输出十六进制
              字宽 16，字高 16（对应 OLED_FONT_16）
       2) 输入要用的汉字，生成字模数组，形如：
              static const uint8_t hz_zhong[32] = {0x00,0x00, ...};
       3) 把汉字和字模按下面结构登记到 s_hz_table[] 里：
              { 0xE4,0xB8,0xAD, (const uint8_t *)hz_zhong },   // "中" 的 UTF-8 编码
       4) 之后即可 OLED_ShowChinese(0, 0, "中", OLED_FONT_16, 1);

     下面预留了查表框架，但默认表为空，直接调用不会显示任何内容，
     也不会报错。这样保证驱动开箱可用，用到中文时再按需添加。
     ======================================================================== */

  (void)x;
  (void)y;
  (void)p_str;
  (void)font_size;
  (void)value;

  /* 示例：登记一个字模后，取消注释下面两行即可显示
  OLED_ShowFontDot(x, y,       &hz_zhong[0],  16U, 8U, value);
  OLED_ShowFontDot(x, y + 8U,  &hz_zhong[8],  16U, 8U, value);
  OLED_ShowFontDot(x + 8U, y,      &hz_zhong[16], 16U, 8U, value);
  OLED_ShowFontDot(x + 8U, y + 8U, &hz_zhong[24], 16U, 8U, value);
  */
}

/* ------------------------------ 滚动 ------------------------------ */

void OLED_ScrollStart(uint8_t left, uint8_t start_page, uint8_t end_page)
{
  if (start_page > 7U)
  {
    start_page = 0U;
  }
  if (end_page > 7U)
  {
    end_page = 7U;
  }
  if (start_page > end_page)
  {
    uint8_t tmp = start_page;
    start_page = end_page;
    end_page = tmp;
  }

  OLED_WriteCmd(0x2EU);   /* 停用滚动，先清干净 */

  /* 0x26 = 向右滚动，0x27 = 向左滚动 */
  OLED_WriteCmd(left ? 0x27U : 0x26U);
  OLED_WriteCmd(0x00);              /* 空字节 */
  OLED_WriteCmd(start_page);        /* 起始页 */
  OLED_WriteCmd(0x00);              /* 滚动速度，越小越快 */
  OLED_WriteCmd(end_page);          /* 结束页 */
  OLED_WriteCmd(0x00);              /* 空字节 */
  OLED_WriteCmd(0xFF);              /* 空字节 */
  OLED_WriteCmd(0x2FU);             /* 启动滚动 */
}

void OLED_ScrollStop(void)
{
  OLED_WriteCmd(0x2EU);
}
