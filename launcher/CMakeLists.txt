cmake_minimum_required(VERSION 3.12)
project(img_trans_launcherUI C)

# ----- 交叉编译基础设置（沿用原工程）-----
set(CMAKE_SYSROOT "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_definitions(-D_GNU_SOURCE)

# ----- 源文件 -----
set(SOURCES
    main.c
    lvgl_init.c
    launcher_ui.c
    system_monitor.c
)

add_executable(${PROJECT_NAME} ${SOURCES})

# ----- 头文件路径 -----
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SYSROOT}/usr/include
    ${CMAKE_SYSROOT}/usr/include/lvgl
    ${CMAKE_SYSROOT}/usr/include/lvgl/lv_drivers
    ${CMAKE_SYSROOT}/usr/include/libdrm
    ${CMAKE_SYSROOT}/usr/include/libevdev-1.0
)

# ----- 链接库 -----
target_link_libraries(${PROJECT_NAME}
    lvgl
    freetype
    png
    z
    drm
    pthread
    m
    dl
    evdev
)