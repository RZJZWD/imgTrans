cmake_minimum_required(VERSION 3.12)
# 定义项目名称变量
set(PROJECT_NAME "capdis")
project(${PROJECT_NAME} C)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/display  # 添加display目录到头文件搜索路径
    ${CMAKE_CURRENT_SOURCE_DIR}/capture

)
# 源文件
set(SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/display/display_rgb.c
    ${CMAKE_CURRENT_SOURCE_DIR}/capture/capture_uvc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/main.c
)
# 显示程序（链接所有必要的库）
add_executable(${PROJECT_NAME} ${SOURCES})
target_link_libraries(${PROJECT_NAME} 
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