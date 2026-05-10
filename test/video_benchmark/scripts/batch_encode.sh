#!/bin/bash
# 批量编码脚本 - 嵌入式固定列版（含 refs）
# 用法: ./scripts/batch_encode.sh

ROOT_DIR="/userdata/video_benchmark"
RAW="${ROOT_DIR}/raw.yuv"
PARAMS_CSV="${ROOT_DIR}/params.csv"

if [ ! -f "$RAW" ]; then
    echo "错误: 找不到 raw.yuv"
    exit 1
fi

TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
OUT_DIR="${ROOT_DIR}/encode_${TIMESTAMP}"
mkdir -p "${OUT_DIR}"

RESULT_CSV="${OUT_DIR}/result.csv"
# 读取表头
read -r HEADER_LINE < "$PARAMS_CSV"
echo "${HEADER_LINE},enc_time_total_sec,frame_count,enc_time_per_frame_sec,avg_cpu_percent" > "$RESULT_CSV"

# 跳过表头，逐行处理
exec 3< "$PARAMS_CSV"
read -r <&3   # 跳过表头

while IFS=',' read -r group preset crf gop bf refs tune profile rest <&3; do
    # 跳过空行
    [ -z "$group" ] && continue

    # 去除前后空格
    group=$(echo "$group" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
    preset=$(echo "$preset" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
    crf=$(echo "$crf" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
    gop=$(echo "$gop" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
    bf=$(echo "$bf" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
    refs=$(echo "$refs" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
    tune=$(echo "$tune" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
    profile=$(echo "$profile" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')

    OUTPUT_NAME="${group}_${preset}_crf${crf}_g${gop}_b${bf}_r${refs}"
    OUTPUT_FILE="${OUT_DIR}/${OUTPUT_NAME}.h264"
    LOG_FILE="${OUT_DIR}/ffmpeg_${group}.log"
    CPU_LOG="${OUT_DIR}/cpu_${group}.log"

    echo "========== 编码第 ${group} 组: ${OUTPUT_NAME} =========="

    CMD="ffmpeg -f rawvideo -pix_fmt yuyv422 -video_size 640x480 -framerate 25 -i ${RAW} -c:v libx264 -preset ${preset} -crf ${crf} -g ${gop} -bf ${bf}"
    [ -n "$refs" ] && CMD="$CMD -refs $refs"
    [ -n "$tune" ] && CMD="$CMD -tune $tune"
    [ -n "$profile" ] && CMD="$CMD -profile:v $profile"
    CMD="$CMD -f h264 -y ${OUTPUT_FILE}"
    echo "执行命令: $CMD"

    START_NS=$(date +%s.%N)
    $CMD > "$LOG_FILE" 2>&1 &
    FFMPEG_PID=$!

    # 可选 CPU 监控
    if command -v pidstat >/dev/null 2>&1; then
        pidstat -p $FFMPEG_PID 1 > "$CPU_LOG" 2>&1 &
        PIDSTAT_PID=$!
    else
        PIDSTAT_PID=""
    fi

    wait $FFMPEG_PID

    if [ -n "$PIDSTAT_PID" ]; then
        kill $PIDSTAT_PID 2>/dev/null
        wait $PIDSTAT_PID 2>/dev/null
    fi

    END_NS=$(date +%s.%N)
    ELAPSED=$(awk -v s="$START_NS" -v e="$END_NS" 'BEGIN {printf "%.6f", e - s}')
    echo "编码总耗时: ${ELAPSED} 秒"

    # 提取帧数
    FRAME_COUNT=$(sed -n 's/.*frame=\s*\([0-9]\+\).*/\1/p' "$LOG_FILE" | tail -1)
    if [ -z "$FRAME_COUNT" ]; then
        FRAME_COUNT="N/A"
        echo "警告: 未能提取帧数，设为 N/A"
    else
        echo "编码帧数: ${FRAME_COUNT}"
    fi

    # 平均每帧时间
    if [ "$FRAME_COUNT" != "N/A" ] && [ "$FRAME_COUNT" -gt 0 ]; then
        AVG_FRAME_TIME=$(awk -v e="$ELAPSED" -v f="$FRAME_COUNT" 'BEGIN {printf "%.6f", e/f}')
    else
        AVG_FRAME_TIME="N/A"
    fi

    # CPU 占用
    AVG_CPU="N/A"
    if [ -f "$CPU_LOG" ] && [ -s "$CPU_LOG" ]; then
        AVG_CPU=$(awk '/^[0-9]/ {sum+=$8; cnt++} END {if(cnt) printf "%.2f", sum/cnt}' "$CPU_LOG")
        echo "平均 CPU 占用: ${AVG_CPU}%"
    fi

    # 写入结果
    LINE="${group},${preset},${crf},${gop},${bf},${refs},${tune},${profile}"
    echo "${LINE},${ELAPSED},${FRAME_COUNT},${AVG_FRAME_TIME},${AVG_CPU}" >> "$RESULT_CSV"
    echo "第 ${group} 组编码完成。"
    echo ""
done

exec 3<&-   # 关闭文件描述符

echo "所有编码完成，结果保存在: ${OUT_DIR}"
echo "最终结果文件: ${RESULT_CSV}"