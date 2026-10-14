# Laser-Engraver 项目长期笔记

## ★ TF 卡驱动现状（2026-10-14 改写，最重要）

**`Core/Src/tf_card.c` 现在是 HAL_SD 版（765 行），不再是寄存器版。**
对外接口 `tf_card.h` 基本没变，所以 `test.c` 的测试台 / OLED 页面不用改。

- **已跑通**：`1 init OK / 2 mbr 55AA / 3 fs FAT16/12 / 4 rep OK /
  5 speed 5730 KB/s`，`bus/clk: 4bit 24 MHz DMA`，`raw: sta 00000000 line 0`
  （零错误，`line 0` = 一个 `TF_MARK()` 都没触发）。
- 旧寄存器版（2800 行）随时可取回：
  `git show b0e6c05:F405RGT6/Core/Src/tf_card.c`
- 本工程现在这些是新加的/新启用的：
  `Core/Src/sdio.c` + `Core/Inc/sdio.h`（CubeMX 生成的 HAL_SD 初始化 +
  `HAL_SD_MspInit`）、`stm32f4xx_hal_conf.h` 里打开了 `HAL_SD_MODULE_ENABLED`、
  `stm32f4xx_it.c` 里有 `SDIO_IRQHandler` + `DMA2_Stream3/6_IRQHandler`、
  `dma.c` 里补了 DMA2 时钟与这两个中断、Keil 工程登记了
  `sdio.c` / `stm32f4xx_hal_sd.c` / `stm32f4xx_ll_sdmmc.c`。
- 排查开关：`tf_card.h` 里 **`TF_USE_HAL_DMA`**（默认 1 = DMA 异步版；
  改 0 = `HAL_SD_ReadBlocks()` 阻塞轮询版，无中断依赖，用来隔离问题）。
- 本版**不做 CMD6 高速模式**（HAL_SD 没有对应 API）→ 最高 24MHz、
  `highSpeedMode` 恒 0、报告里 `HS` 显示 `--`，`TF_SetSpeed(48M)` 按 24M 处理。
- **`g_tfLastSta` 的含义变了**：从 `SDIO->STA` 变成 **`hsd.ErrorCode`**。
- `TF_INCLUDE_TEST` 已置 0（旧版那套日志轮播没重写，测试统一走 `test.c`）。

★ **教训**：寄存器版走了 4 轮硬件往返才定位到一半问题（ACMD41 的 R3 无 CRC7、
CSD v1.0 位域、DMA 停流竞态、FIFO 残余），HAL_SD 版**一次就过**。
下次遇到 SD/TF 卡，先考虑直接上 `HAL_SD`。细节见技能 `stm32-sdio-tf-card`。

## 结构

- `F405RGT6/` —— STM32F405RGT6 下位机（CubeMX + Keil MDK，HAL 库，裸机）。
  固件在 `Core/Src`，Keil 工程在 `MDK-ARM/JiGuang.uvprojx`。
- `Qt/` —— 上位机（Qt 5.8 MinGW 32bit）。
- 调试入口集中在 `Core/Src/test.c` + `Core/Inc/test.h`：每个 `test_xxx()` 都以
  `while(1)` 结尾，在 main.c 的 `USER CODE BEGIN 2` 里手动调用，一次只开一个。

## 约定 / 坑

- 行尾不统一：`Core/Src/test.c`、`Core/Inc/test.h` 是 CRLF（`core.autocrlf=true`，
  无 .gitattributes）；`Core/Src/tf_card.c` 是 LF。改文件前后都要查行尾，别整文件重写。
- `MDK-ARM/JiGuang.uvprojx` 由 Keil 维护，新增源文件要加进去（Git 里会显示为改动）。
- 语法自检可行（不需要 Keil）：
  `arm-none-eabi-gcc -fsyntax-only -mcpu=cortex-m4 -mthumb -DUSE_HAL_DRIVER -DSTM32F405xx -Wall -Wextra -ICore/Inc -IDrivers/STM32F4xx_HAL_Driver/Inc -IDrivers/STM32F4xx_HAL_Driver/Inc/Legacy -IDrivers/CMSIS/Device/ST/STM32F4xx/Include -IDrivers/CMSIS/Include Core/Src/xxx.c`

## 已修 bug（2026-10-14，★ 属于已被取代的寄存器版）

> ⚠ 下面这些坑都是**寄存器版**踩的。该版本已被 HAL_SD 版取代（见最上面一节），
> 但**坑本身依然有参考价值** —— 里面的芯片行为、位域、时序结论都是实测出来的，
> 将来写别的 SDIO/裸寄存器驱动时照样适用。

- **`tf_card.c` 识别卡死（提交 7e210ae + 66139a3）**：`tf_cmd()` 对 ACMD41 的 R3 响应也检查
  CCRCFAIL —— 但 R3 按规范没有 CRC7 字段，SDIO 必然报 CCRCFAIL。
  ★★ 关键细节：这种场合外设**只置 CCRCFAIL，不置 CMDREND**，所以
  「CCRCFAIL 就是响应已到达的信号」——既不能当错误，也不能清完标志再等 CMDREND
  （那样会误报 cTO）。正确做法：放宽场合（R2 长响应 / R3 用 no_crc）清掉标志后
  **直接当成功返回**。ST 的 HAL 在 `SDMMC_GetCmdResp2/3()` 里同样只清标志。
- `g_tfLastResp` 原来只在 `tf_check_r1()` 里写，所以别拿它当"响应原值"看
  （没出错时它一直是 0）。现在 CCRCFAIL 分支也会记 `SDIO->RESP1`。
- `TF_ACMD41_TRIES` 由 `0x000FFFFF`(≈17 分钟) 改成 `4000`(≈4 秒)。
- `TF_Init()` 失败收尾要连 `SDIO->CLKCR` 一起清，只清 `POWER` 的话
  SDIO_CK 会一直在 PC12 上跑 400kHz（干扰源）。
- `TF_Test()` 吞吐测试越界（256 块 128KB 读进 512B 的 `s_tfBuf`，踩烂 SRAM1）。
  已改成 `s_tfBig[4KB]` + `TF_PERF_BLOCKS=8` / `TF_PERF_ROUNDS=32` 分块读。提交 `db8cd24`。
- `TF_R1_ERR_MASK` 由 `0x000000FF` 改为 `0xFDF98080`（低 8 位基本是保留位，
  真错误位在 19~31）。同一次提交。
- **CSD 容量算成 0 → 判 notSD（提交 6739b70）**：`tf_parse_csd()` 的 v1.0 分支
  把 READ_BL_LEN 写成 `csd[1] & 0xF`（= CSD bits 67:64，其实落在 C_SIZE 里），
  导致"块大小"由 C_SIZE 的低 4 位决定，≤ 8 时 `bl_size/512 == 0` → `blockCount = 0`。
  ★★ **CSD v1.0 与 v2.0 的 `C_SIZE` 位置不同**，这是最阴的坑：
  ```
                     CSD v1.0                CSD v2.0
    CSD_STRUCTURE    bits 127:126 = 0        bits 127:126 = 1
    READ_BL_LEN      bits 83:80              bits 83:80（两版相同）
    C_SIZE           bits 73:62 (12 位)      bits 69:48 (22 位)
    C_SIZE_MULT      bits 49:47              —— 没有
  ```
  验证：2GB SDSC 用 `(C_SIZE+1)*2^(MULT+2)*2^BL`，MULT=7/BL=10 → C_SIZE=4095=0xFFF，
  正好卡满 12 位。v2.0 容量 = `(C_SIZE+1)*512KB`。
  版本判据现在以 CSD 自己的 bits 127:126 为准（不再只看 OCR 的 CCS 位），
  并顺手纠正 `g_tf.cardType`。`TF_Info` 加了 `csd[4]` 存原始 CSD，
  串口报告有 `csd : ...` 一行，容量可疑时直接手工解码。
- ★ **诊断变量要认清**：`TF_MARK` 只记行号。`g_tfLastSta` / `g_tfLastResp`
  只在出错分支赋值，所以成功路径上它们是**上一次**的值（`resp 80FF8000`
  其实是很早以前 ACMD41 的 OCR，不是 CSD —— 别拿它当响应看）。
- ★★ **SDIO(DMA) 读：块尾必须由 CPU 补回 —— FIFO 残余**（提交 b0e6c05，2026-10-14）
  **SDIO 的 RX DMA 请求是按"RX FIFO 攒到 8 个字"（`RXFIFOHF`）发的。**
  数据阶段结束时 FIFO 里往往还剩最后几个字（不足 8 个），这个请求就再也
  不来了 → `NDTR` 永远停在非 0 上 → **块尾那几十个字节留在 FIFO 里被丢掉**。
  ★ 而且**完全不报错**：DMA 模式下长度检查被跳过、数据 CRC 是硬件按进了
  FIFO 的字节算的（`DBCKEND` 照样置位）、反复读同一块每次都丢同样多
  → 一致性测试也看不出。表现 = "块尾是缓冲区里的旧数据"。
  典型症状：MBR 的 `55AA` 没了（块 0 的 510/511 正好是最后 2 字节），
  但同一块靠前的内容全对 —— 看着像"这张卡没有 MBR"。
  → **修法**：`tf_dma_stop_and_drain()`。停流后拿 `NDTR` 和
  **`SDIO->FIFOCNT`（offset 0x48，只读，直接给出 FIFO 里的字数）** 对账，
  一致才把这几个字按顺序补到缓冲区末尾；对不上就报错，绝不瞎填。
  ⚠ 注意："等 `NDTR` 归零"是**没用的**（试过，500ms 也等不到）——
  请求根本不会再来了，不是慢。
  ⚠ 写方向没有这个问题（FIFO 是 DMA 灌给 SDIO 的，`DATAEND` 时必然已喂完）。
- ⚠ **STM32F4 的 `SDIO_STA` 位排布和 F1 不同，别按印象记**：
  ```
  bit0 CCRCFAIL  bit1 DCRCFAIL  bit2 CTIMEOUT  bit3 DTIMEOUT
  bit4 TXUNDERR  bit5 RXOVERR   bit6 CMDREND   bit7 CMDSENT
  bit8 DATAEND   bit9 STBITERR  bit10 DBCKEND  bit11 CMDACT ...
  ```
  所以 `sta = 0x04` 是 **CTIMEOUT**，不是 CMDREND（我第一轮记错、误判过一次）。
- ★ **48MHz 这一档在杜邦线上基本必挂**：`TF_Init()` 里速度协商从 48M 往下试，
  48M 失败（`CTIMEOUT`，`line 506`）是**预期**的，会落到 24MHz。
  所以报告里的 `raw` 行是"最后一次见到的错"，成功时也非零，别当故障。
  ★ 反过来看 `bus/clk`：正常应落到 **24MHz**；如果掉到 **4MHz**
  （`tf_negotiate_speed()` 的兜底档），说明**每一档的试读都失败了** ——
  这是"读有问题"的强信号，别忽略。

## 板子上的串口分工（重要）

- **USART3 = 蓝牙口**（`USART3_SendDMA` / `USART3_RxStart`，115200 8N1），
  上位机通信也走它。调试时（不跑上位机流程）它是空着的，可以拿来打日志。
- USART1 = PA9/PA10，115200，但 PA9 不一定引出来了，别默认能用。
- OLED 在 I2C1 (PB6/PB7) @400kHz，整屏刷新 1024 字节约 25~35ms
  —— 翻页时那一下"半屏新半屏旧"的观感就来自这里，不是显存被踩。



## 调试入口分工

- `test.c::test_tf_card()` —— 逐项显示（P0 结论 / P1 五项检查 / P2 吞吐+原始诊断），
  main.c 里目前调的是它。自己按正确尺寸测，不依赖 `TF_Test()`。
- `tf_card.c::TF_Test(doWriteTest)` —— 驱动自带自检，报告进日志缓冲后 OLED 分页轮播，
  现在是安全的（修好越界之后）。两者都可以用。

