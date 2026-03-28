# CMakeLists.txt
cmake_minimum_required(VERSION 3.16)

# 设置项目名称
set(PROJECT_NAME tusb_hid)

# 包含ESP-IDF构建系统
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 初始化项目
project(${PROJECT_NAME})