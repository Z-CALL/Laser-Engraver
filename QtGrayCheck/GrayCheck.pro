#-------------------------------------------------
# GrayCheck —— 灰度数组反解 / 回环校验工具
#
# 用途：把上位机「导出灰度C数组」产出的文件读回来，还原成图像，
#       用来判断"图像 -> 灰度 -> 数组"这条链路有没有问题。
#       链路错 → 反解结果不对；链路对 → 问题只可能在串口/固件/机械。
#
# 构建（Qt 5.8 + MinGW 32bit）：
#   qmake GrayCheck.pro -spec win32-g++
#   mingw32-make -j4
# 命令行用法：
#   GrayCheck.exe <导出的数组文件> [输出png]
# 不带参数则打开文件选择框。
#-------------------------------------------------

QT       += core gui

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET   = GrayCheck
TEMPLATE = app
CONFIG  += c++11

SOURCES += main.cpp \
           graydecode.cpp \
           graywindow.cpp

HEADERS += graydecode.h \
           graywindow.h
