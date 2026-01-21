cmake_minimum_required(VERSION 3.12)

# # 启用编译数据库
# set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 定义项目名称变量
set(PROJECT_NAME "capdis")
project(${PROJECT_NAME} C)

# 设置C标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 交叉编译设置
set(CMAKE_SYSROOT "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/display
    ${CMAKE_CURRENT_SOURCE_DIR}/capture
)

# 查找 JPEG 库
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
    ${JPEG_LIBRARY}
)

# # 生成 compile_commands.json 的软链接
# configure_file(
#     "${CMAKE_BINARY_DIR}/compile_commands.json"
#     "${CMAKE_SOURCE_DIR}/compile_commands.json"
#     COPYONLY
# )