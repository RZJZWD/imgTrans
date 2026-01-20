cmake_minimum_required(VERSION 3.12)

project(uvc_lvgl C)

# 设置C标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 交叉编译标志
set(CMAKE_SYSROOT "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# 包含目录（指向目标系统的头文件）
include_directories(
    ${CMAKE_SYSROOT}/usr/include
    ${CMAKE_SYSROOT}/usr/include/lvgl
    ${CMAKE_SYSROOT}/usr/include/lvgl/lv_drivers
    ${CMAKE_SYSROOT}/usr/include/libdrm
    ${CMAKE_SYSROOT}/usr/include/libevdev-1.0
    ${CMAKE_SYSROOT}/usr/include/rockchip/
)

# 捕获程序
#add_executable(uvc_capture capture.c)
#target_link_libraries(uvc_capture v4l2 m)
# 显示和捕获程序（链接所有必要的库）
add_executable(uvc_display display.c)
add_executable(uvc_capture capture.c)
target_link_libraries(uvc_display 
    lvgl 
    freetype 
    png 
    z 
    drm 
    pthread 
    m 
    dl
)

# 不安装到主机，只构建