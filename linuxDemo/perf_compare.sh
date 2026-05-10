#!/bin/bash

# ==================== 配置区 ====================
MULTI_EXE="./img_trans"           # 多线程版可执行文件
SINGLE_EXE="./img_trans_single"   # 单线程版可执行文件
DURATION=30                       # 每个测试时长（秒）
# 默认参数（可根据实际修改，或通过命令行传入）
ARGS="-c 640 480 25 yuyv -s 0 -o 1 output.mp4"

# perf 事件列表（基于 perf list 输出）
PERF_EVENTS="cycles,instructions,cache-misses,context-switches,cpu-migrations,task-clock,page-faults"

# 报告文件（带时间戳）
REPORT_FILE="performance_report_$(date +%Y%m%d_%H%M%S).txt"

# 如果脚本带有参数，则用它们覆盖默认参数
if [ $# -gt 0 ]; then
    ARGS="$@"
fi
# ===============================================

# 临时文件
SINGLE_LOG=$(mktemp)
MULTI_LOG=$(mktemp)
SINGLE_PERF=$(mktemp)
MULTI_PERF=$(mktemp)

# 清理函数
cleanup() {
    rm -f $SINGLE_LOG $MULTI_LOG $SINGLE_PERF $MULTI_PERF
}
trap cleanup EXIT

# 函数：运行单个测试
# 参数: $1 - 程序名（用于显示） $2 - 可执行文件路径 $3 - 日志文件 $4 - perf输出文件
run_test() {
    local name=$1
    local exe=$2
    local logfile=$3
    local perffile=$4

    echo "=========================================="
    echo "开始测试 $name ...($DURATION s)"
    echo "命令: $exe $ARGS"
    echo "=========================================="

    # 启动程序
    $exe $ARGS > "$logfile" 2>&1 &
    local pid=$!

    # 启动 perf stat 监控
    perf stat -p $pid -e $PERF_EVENTS -o "$perffile" &
    local perf_pid=$!

    # 等待测试时长
    sleep $DURATION

    # 停止程序
    kill $pid 2>/dev/null
    wait $pid 2>/dev/null
    wait $perf_pid 2>/dev/null

    echo "测试 $name 完成。"
}

# 开始写入报告文件
{
    echo "==================== 性能对比测试报告 ===================="
    echo "测试时间: $(date)"
    echo "每个测试时长: ${DURATION} 秒"
    echo "单线程程序: $SINGLE_EXE"
    echo "多线程程序: $MULTI_EXE"
    echo "命令行参数: $ARGS"
    echo "perf 事件: $PERF_EVENTS"
    echo "==========================================================="
    echo ""
} | tee "$REPORT_FILE"

# 1. 测试单线程版本
run_test "单线程" "$SINGLE_EXE" "$SINGLE_LOG" "$SINGLE_PERF"

# 2. 测试多线程版本
run_test "多线程" "$MULTI_EXE" "$MULTI_LOG" "$MULTI_PERF"

# 汇总数据
{
    echo ""
    echo "==================== 应用层性能汇总 ===================="

    # 单线程应用层信息
    echo "--- 单线程 ---"
    grep "Performance(" "$SINGLE_LOG" | tail -n 5

    # 获取最后一行性能数据，提取字段
    last_single=$(grep "Performance(" "$SINGLE_LOG" | tail -1)
    avg_single=$(echo "$last_single" | awk -F'avg fps=' '{print $2}' | awk '{print $1}')
    drop_single=$(echo "$last_single" | sed -n 's/.*dropped=\([0-9]\+\).*/\1/p')
    queue_single=$(echo "$last_single" | sed -n 's/.*queue=\([0-9]\+\).*/\1/p')
    free_single=$(echo "$last_single" | sed -n 's/.*free_pool=\([0-9]\+\).*/\1/p')
    echo "avg fps: $avg_single"
    echo "dropped: $drop_single"
    echo "queue: $queue_single"
    echo "free_pool: $free_single"

    echo ""
    echo "--- 多线程 ---"
    grep "Performance(" "$MULTI_LOG" | tail -n 5

    last_multi=$(grep "Performance(" "$MULTI_LOG" | tail -1)
    avg_multi=$(echo "$last_multi" | awk -F'avg fps=' '{print $2}' | awk '{print $1}')
    drop_multi=$(echo "$last_multi" | sed -n 's/.*dropped=\([0-9]\+\).*/\1/p')
    queue_multi=$(echo "$last_multi" | sed -n 's/.*queue=\([0-9]\+\).*/\1/p')
    free_multi=$(echo "$last_multi" | sed -n 's/.*free_pool=\([0-9]\+\).*/\1/p')
    echo "avg fps: $avg_multi"
    echo "dropped: $drop_multi"
    echo "queue: $queue_multi"
    echo "free_pool: $free_multi"

    echo ""
    echo "==================== perf stat 原始输出 ===================="
    echo "--- 单线程 perf stat ---"
    cat "$SINGLE_PERF"
    echo ""
    echo "--- 多线程 perf stat ---"
    cat "$MULTI_PERF"

    echo ""
    echo "==================== 关键指标对比 ===================="
    # 解析 perf 指标（兼容 BusyBox grep）
    # cycles, instructions, cache-misses, context-switches, cpu-migrations, page-faults 直接取第一个数字
    for metric in cycles instructions cache-misses context-switches cpu-migrations page-faults; do
        single_val=$(grep -E "[0-9,]+ +$metric" "$SINGLE_PERF" | head -1 | awk '{print $1}' | tr -d ',')
        multi_val=$(grep -E "[0-9,]+ +$metric" "$MULTI_PERF" | head -1 | awk '{print $1}' | tr -d ',')
        echo "$metric: 单线程=${single_val:-N/A}, 多线程=${multi_val:-N/A}"
    done

    # task-clock 特殊处理（可能有单位）
    single_task=$(grep "task-clock" "$SINGLE_PERF" | head -1 | awk '{print $1}')
    multi_task=$(grep "task-clock" "$MULTI_PERF" | head -1 | awk '{print $1}')
    echo "task-clock: 单线程=${single_task:-N/A}, 多线程=${multi_task:-N/A}"

    # 计算 IPC
    single_cyc=$(grep -E "[0-9,]+ +cycles" "$SINGLE_PERF" | head -1 | awk '{print $1}' | tr -d ',')
    single_ins=$(grep -E "[0-9,]+ +instructions" "$SINGLE_PERF" | head -1 | awk '{print $1}' | tr -d ',')
    multi_cyc=$(grep -E "[0-9,]+ +cycles" "$MULTI_PERF" | head -1 | awk '{print $1}' | tr -d ',')
    multi_ins=$(grep -E "[0-9,]+ +instructions" "$MULTI_PERF" | head -1 | awk '{print $1}' | tr -d ',')
    if [[ -n $single_cyc && $single_cyc -gt 0 ]]; then
        single_ipc=$(echo "scale=2; $single_ins / $single_cyc" | bc)
    else
        single_ipc="N/A"
    fi
    if [[ -n $multi_cyc && $multi_cyc -gt 0 ]]; then
        multi_ipc=$(echo "scale=2; $multi_ins / $multi_cyc" | bc)
    else
        multi_ipc="N/A"
    fi
    echo "IPC (instructions/cycles): 单线程=${single_ipc}, 多线程=${multi_ipc}"

    echo ""
    echo "测试完成。报告已保存至: $REPORT_FILE"
} | tee -a "$REPORT_FILE"