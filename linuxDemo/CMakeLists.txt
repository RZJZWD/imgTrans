cmake_minimum_required(VERSION 3.12)

# # 启用编译数据库
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 定义项目名称变量
set(PROJECT_NAME "capdis")
project(${PROJECT_NAME} C)
set(CMAKE_BUILD_TYPE "Debug")
# 设置C标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 交叉编译设置
set(CMAKE_SYSROOT "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# ========== 条件编译选项 ==========
option(BUILD_MAIN "Build main program capdis" ON)
option(BUILD_TEST1 "Build test program uvc_video" ON)
option(BUILD_TEST2 "Build test program memmantest" ON)

# 查找 JPEG 库
find_library(JPEG_LIBRARY NAMES jpeg libjpeg)
if(JPEG_LIBRARY)
    message(STATUS "找到JPEG库: ${JPEG_LIBRARY}")
else()
    message(WARNING "未找到JPEG库，将无法保存/显示JPEG图像")
endif()

# ========== 主程序 capdis ==========
if(BUILD_MAIN)
    # 源文件
    set(SOURCES
        display/display_rgb.c
        capture/capture_uvc.c
        capture/convert.c
        main.c
    )

    # 创建可执行文件
    add_executable(${PROJECT_NAME} ${SOURCES})

    # 包含目录
    target_include_directories(${PROJECT_NAME} PRIVATE
        ${CMAKE_SYSROOT}/usr/include
        ${CMAKE_SYSROOT}/usr/include/lvgl
        ${CMAKE_SYSROOT}/usr/include/lvgl/lv_drivers
        ${CMAKE_SYSROOT}/usr/include/libdrm
        ${CMAKE_SYSROOT}/usr/include/libevdev-1.0
        ${CMAKE_SYSROOT}/usr/include/rockchip/
        ${CMAKE_SYSROOT}/usr/include/rga
        ${CMAKE_CURRENT_SOURCE_DIR}/display
        ${CMAKE_CURRENT_SOURCE_DIR}/capture
        # ${CMAKE_CURRENT_SOURCE_DIR}/dmabuf_manager  # 添加dmabuf_manager头文件路径
    )

    # 链接库
    target_link_libraries(${PROJECT_NAME} 
        lvgl 
        freetype 
        png 
        z 
        drm 
        pthread 
        m 
        dl 
        rga
        ${JPEG_LIBRARY}
    )
    
    message(STATUS "主程序 capdis 已启用编译")
endif()

# ========== 测试程序1: uvc_video ==========
if(BUILD_TEST1)
    # 测试程序的源文件
    set(TEST1_SOURCES
        videoDemo.c
        # capture/capture_uvc.c
        # capture/convert.c
        encode_to_video/encode_to_video.c
    )
    add_executable(uvc_video ${TEST1_SOURCES})

    find_library(AVFORMAT_LIBRARY avformat)
    find_library(AVCODEC_LIBRARY avcodec)
    find_library(AVUTIL_LIBRARY avutil)
    find_library(SWSCALE_LIBRARY swscale)
    
    target_include_directories(uvc_video PRIVATE
        ${CMAKE_SYSROOT}/usr/include
        ${CMAKE_SYSROOT}/usr/include/rga
        # ${CMAKE_CURRENT_SOURCE_DIR}/capture
        ${CMAKE_CURRENT_SOURCE_DIR}/encode_to_video

        # 显式添加 FFmpeg 头文件路径（即使已在 sysroot/usr/include 下，双重保险）
        ${CMAKE_SYSROOT}/usr/include/libavcodec
        ${CMAKE_SYSROOT}/usr/include/libavformat
        ${CMAKE_SYSROOT}/usr/include/libavutil
        ${CMAKE_SYSROOT}/usr/include/libswscale
        ${CMAKE_SYSROOT}/usr/include/libswresample
    )
    
    target_link_libraries(uvc_video
        pthread
        m
        rga
        ${AVFORMAT_LIBRARY}
        ${AVCODEC_LIBRARY}
        ${AVUTIL_LIBRARY}
        ${SWSCALE_LIBRARY}
        ${V4L2_LIBRARY}
        ${JPEG_LIBRARY}
    )
    
    message(STATUS "测试程序1 uvc_video 已启用编译")
endif()

# ========== 测试程序2: memmantest ==========
if(BUILD_TEST2)
    set(TEST2_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/memmantest.c
        ${CMAKE_CURRENT_SOURCE_DIR}/dmabuf_manager/dmabuf_manager.c
        ${CMAKE_CURRENT_SOURCE_DIR}/capture/convert.c
        ${CMAKE_CURRENT_SOURCE_DIR}/capture/capture_uvc.c
        ${CMAKE_CURRENT_SOURCE_DIR}/display/display_rgb.c
    )
    
    add_executable(memmantest ${TEST2_SOURCES})
    
    # 设置测试程序的包含目录
    target_include_directories(memmantest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/dmabuf_manager
        ${CMAKE_CURRENT_SOURCE_DIR}/capture
        ${CMAKE_CURRENT_SOURCE_DIR}/display
        ${CMAKE_SYSROOT}/usr/include/rga
        ${CMAKE_SYSROOT}/usr/include/lvgl
        ${CMAKE_SYSROOT}/usr/include/lvgl/lv_drivers
        ${CMAKE_SYSROOT}/usr/include/libdrm
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    
    target_link_libraries(memmantest
        m
        rga
        lvgl
        drm
        freetype
        png
        ${JPEG_LIBRARY}
    )
    
    message(STATUS "测试程序2 memmantest 已启用编译")
endif()

# 如果没有启用任何目标，显示警告
if(NOT BUILD_MAIN AND NOT BUILD_TEST1 AND NOT BUILD_TEST2)
    message(WARNING "所有构建目标都已禁用，不会生成任何可执行文件")
endif()