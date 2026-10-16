#!/bin/sh
# 主机端验证 tf_fat.c / tf_image.c —— 不用烧板子
#
# 原理：把 TF_ReadBlocks() 桩到一张本地磁盘镜像上，然后用真源码跑
#       "挂 FAT -> 找文件 -> 读整份 -> 校验 CRC32"。
#
# 用法：  sh run.sh
# 需要：  Qt 自带的 mingw gcc（可用环境变量 GCC 覆盖）
#
# ★ 全程用【相对路径】+ 先 cd 进来 —— 这样 Windows 版的 gcc/python 也不会
#   把 /c/... 这种 Git Bash 路径理解错。
set -e
cd "$(dirname "$0")"

GCC=${GCC:-/c/Qt/Qt5.8.0/Tools/mingw530_32/bin/gcc.exe}
PY=${PY:-python}
SRC=../../Core/Src
INC=../../Core/Inc

# 影子头目录：真工程的 main.h 会拉进整个 HAL，这里换成一个只含 stdint 的桩
# （★ 引号包含会先搜"包含者所在目录"，所以必须让 main.h 从 -I 路径里被找到）
rm -rf shadow && mkdir -p shadow
cp "$INC/tf_card.h" "$INC/tf_fat.h" "$INC/tf_image.h" shadow/
printf '#ifndef __STUB_MAIN_H\n#define __STUB_MAIN_H\n#include <stdint.h>\n#include <stddef.h>\n#endif\n' > shadow/main.h

"$PY" mkimg.py   disk.img
"$PY" mkimg32.py disk32.img

echo
echo "########## FAT16 ##########"
"$GCC" -O1 -Wall -Wextra -Ishadow "$SRC/tf_fat.c" "$SRC/tf_image.c" harness.c -o t16.exe
./t16.exe disk.img | tail -9

echo
echo "########## FAT32 ##########"
"$GCC" -O1 -Wall -Wextra -Ishadow "$SRC/tf_fat.c" "$SRC/tf_image.c" harness.c -o t32.exe
./t32.exe disk32.img 33 17 561 42EA2DC7 577 3 32 | tail -9

echo
echo "########## FAT16 + 512B 环形缓冲（极限回绕）##########"
"$GCC" -O1 -DTFIMG_RING_SIZE=512 -Ishadow "$SRC/tf_fat.c" "$SRC/tf_image.c" harness.c -o t512.exe
./t512.exe disk.img | tail -4
