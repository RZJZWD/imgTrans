cmake_minimum_required(VERSION 3.12)

project(uvc_lvgl C)

# 设置C标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 交叉编译设置
set(CMAKE_SYSROOT "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# 包含目录
include_directories(
    ${CMAKE_SYSROOT}/usr/include
    ${CMAKE_SYSROOT}/usr/include/lvgl
    ${CMAKE_SYSROOT}/usr/include/lvgl/lv_drivers
    ${CMAKE_SYSROOT}/usr/include/libdrm
    ${CMAKE_SYSROOT}/usr/include/rga
    ${CMAKE_SYSROOT}/usr/include/libevdev-1.0
    ${CMAKE_SYSROOT}/usr/include/rockchip/
)

# 查找JPEG库
find_library(JPEG_LIBRARY
    NAMES jpeg libjpeg.so.62 libjpeg.so
    PATHS ${CMAKE_SYSROOT}/usr/lib ${CMAKE_SYSROOT}/lib
    REQUIRED
)

if(JPEG_LIBRARY)
    message(STATUS "找到JPEG库: ${JPEG_LIBRARY}")
else()
    message(WARNING "未找到JPEG库，将无法保存/显示JPEG图像")
endif()


# 查找核心库
# find_library(JPEG_LIBRARY jpeg)

find_library(V4L2_LIBRARY v4l2)


# 捕获程序
add_executable(uvc_capture capture.c)
target_link_libraries(uvc_capture 
    m
    ${JPEG_LIBRARY}
)
# 捕获程序rga
add_executable(uvc_rga capture_rga.c)
target_link_libraries(uvc_rga
    m
    rga 
    ${JPEG_LIBRARY}
)

# 显示程序
add_executable(uvc_display display.c)
target_link_libraries(uvc_display 
    lvgl 
    freetype 
    png 
    z 
    drm 
    pthread 
    m 
    dl 
    ${JPEG_LIBRARY}
)
