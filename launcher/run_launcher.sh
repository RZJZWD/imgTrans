#!/bin/sh

# ============================================
# img_trans_launcherUI 守护启动脚本
# 用法: ./run_launcher.sh [img_trans路径] [位置名称]
# ============================================

# 默认值
DEF_IMG_PATH="./img_trans"
DEF_LOCATION="机位A"

# 获取脚本所在目录（确保启动器路径正确）
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LAUNCHER_BIN="${SCRIPT_DIR}/img_trans_launcherUI"

# 参数处理
if [ $# -eq 0 ]; then
    echo "========================================"
    echo "  img_trans 启动器守护脚本"
    echo "  (未指定参数，使用默认配置)"
    echo "  img_trans 路径: $DEF_IMG_PATH"
    echo "  位置名称      : $DEF_LOCATION"
    echo "========================================"
    IMG_PATH="$DEF_IMG_PATH"
    LOCATION="$DEF_LOCATION"
elif [ $# -eq 2 ]; then
    IMG_PATH="$1"
    LOCATION="$2"
    echo "使用自定义参数: img_trans=$IMG_PATH, 位置=$LOCATION"
else
    echo "用法: $0 [img_trans路径] [位置名称]"
    echo "示例: $0 ./img_trans '机位A'"
    echo "      $0                  (使用默认值)"
    exit 1
fi

# 检查启动器是否存在
if [ ! -x "$LAUNCHER_BIN" ]; then
    echo "错误: 未找到启动器可执行文件 $LAUNCHER_BIN"
    exit 1
fi

# 检查 img_trans 是否存在（警告但不退出）
if [ ! -x "$IMG_PATH" ]; then
    echo "警告: 未找到 img_trans ($IMG_PATH)，启动器可能无法正常拉起"
fi

# 无限循环守护
while true; do
    echo "启动 img_trans_launcherUI ..."
    "$LAUNCHER_BIN" "$IMG_PATH" "$LOCATION"
    echo "启动器已退出，3 秒后重启..."
    sleep 3
done