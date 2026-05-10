#!/usr/bin/env python3
"""
PC 端质量计算脚本 (稳定版)
用法: python3 calc_quality.py encode_20260510_194856
"""
import os
import sys
import csv
import subprocess
import re
import tempfile

# ========== 配置 ==========
RAW_FILE = "raw.yuv"
WIDTH, HEIGHT = 640, 480
PIX_FMT_RAW = "yuyv422"
FRAMERATE = 25                # 必须与采集、编码一致
# =========================

def compute_quality(encoded_file, tmp_dir):
    """计算 PSNR 和 SSIM，使用日志文件提取最终结果"""
    psnr_log = os.path.join(tmp_dir, "psnr.log")
    ssim_log = os.path.join(tmp_dir, "ssim.log")

    # ---- PSNR ----
    psnr_cmd = [
        "ffmpeg", "-hide_banner", "-y",
        "-f", "rawvideo", "-pix_fmt", PIX_FMT_RAW,
        "-video_size", f"{WIDTH}x{HEIGHT}",
        "-framerate", str(FRAMERATE),
        "-i", RAW_FILE,
        "-i", encoded_file,
        "-lavfi", f"[0:v]format=yuv420p,setpts=PTS-STARTPTS[ref];"
                  f"[1:v]setpts=PTS-STARTPTS[enc];"
                  f"[ref][enc]psnr=stats_file={psnr_log}:shortest=1",
        "-f", "null", "-"
    ]
    subprocess.run(psnr_cmd, capture_output=True, timeout=120)

    # ---- SSIM ----
    ssim_cmd = [
        "ffmpeg", "-hide_banner", "-y",
        "-f", "rawvideo", "-pix_fmt", PIX_FMT_RAW,
        "-video_size", f"{WIDTH}x{HEIGHT}",
        "-framerate", str(FRAMERATE),
        "-i", RAW_FILE,
        "-i", encoded_file,
        "-lavfi", f"[0:v]format=yuv420p,setpts=PTS-STARTPTS[ref];"
                  f"[1:v]setpts=PTS-STARTPTS[enc];"
                  f"[ref][enc]ssim=stats_file={ssim_log}:shortest=1",
        "-f", "null", "-"
    ]
    subprocess.run(ssim_cmd, capture_output=True, timeout=120)

    # ---- 解析日志文件 ----
    psnr = extract_psnr_from_log(psnr_log)
    ssim = extract_ssim_from_log(ssim_log)
    return psnr, ssim

def extract_psnr_from_log(log_file):
    """从 psnr 统计文件最后一行提取平均值"""
    if not os.path.exists(log_file):
        return {}
    with open(log_file, 'r') as f:
        lines = f.readlines()
    if not lines:
        return {}
    # 最后一行格式: n:frame mse_y:... mse_u:... mse_v:... mse_avg:... psnr_y:xx.xx psnr_u:xx.xx psnr_v:xx.xx psnr_avg:xx.xx
    last = lines[-1].strip()
    m = re.search(r'psnr_y:(\S+)\s+psnr_u:(\S+)\s+psnr_v:(\S+)\s+psnr_avg:(\S+)', last)
    if m:
        return {
            'psnr_y': float(m.group(1)),
            'psnr_u': float(m.group(2)),
            'psnr_v': float(m.group(3)),
            'psnr_avg': float(m.group(4))
        }
    return {}

def extract_ssim_from_log(log_file):
    """从 ssim 统计文件最后一行提取平均值"""
    if not os.path.exists(log_file):
        return {}
    with open(log_file, 'r') as f:
        lines = f.readlines()
    if not lines:
        return {}
    # 最后一行格式: n:frame Y:0.9 U:0.9 V:0.9 All:0.9 (approx)
    last = lines[-1].strip()
    m = re.search(r'Y:(\S+)\s+U:(\S+)\s+V:(\S+)\s+All:(\S+)', last)
    if m:
        return {
            'ssim_y': float(m.group(1)),
            'ssim_u': float(m.group(2)),
            'ssim_v': float(m.group(3)),
            'ssim_all': float(m.group(4))
        }
    return {}

def process_encode_folder(encode_dir):
    result_csv_path = os.path.join(encode_dir, "result.csv")
    if not os.path.exists(result_csv_path):
        print(f"未找到 {result_csv_path}")
        return

    with open(result_csv_path, 'r', newline='') as f:
        reader = csv.reader(f)
        rows = list(reader)
    if not rows:
        return

    header = rows[0]
    try:
        group_idx = header.index('group')
    except ValueError:
        print("CSV 缺少 'group' 列")
        return

    quality_cols = ['psnr_y','psnr_u','psnr_v','psnr_avg','ssim_y','ssim_u','ssim_v','ssim_all']
    for col in quality_cols:
        if col not in header:
            header.append(col)

    # 创建临时目录存放日志
    with tempfile.TemporaryDirectory() as tmp_dir:
        for i in range(1, len(rows)):
            row = rows[i]
            if len(row) < group_idx+1:
                continue
            group_id = row[group_idx].strip()
            encoded_file = None
            for fname in os.listdir(encode_dir):
                if fname.startswith(f"{group_id}_") and fname.endswith(".h264"):
                    encoded_file = os.path.join(encode_dir, fname)
                    break
            if not encoded_file:
                print(f"组 {group_id}: 找不到 H.264 文件，跳过")
                continue

            print(f"计算组 {group_id}: {os.path.basename(encoded_file)}")
            psnr_vals, ssim_vals = compute_quality(encoded_file, tmp_dir)

            # 填回 CSV
            while len(row) < len(header):
                row.append("")
            for col, val in psnr_vals.items():
                if col in header:
                    row[header.index(col)] = val
            for col, val in ssim_vals.items():
                if col in header:
                    row[header.index(col)] = val

    with open(result_csv_path, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerows(rows)
    print(f"更新完成: {result_csv_path}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("用法: python3 calc_quality.py encode_20260510_194856")
        sys.exit(1)
    target_dir = sys.argv[1]
    os.chdir(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    if not os.path.exists(RAW_FILE):
        print(f"错误: 根目录找不到 {RAW_FILE}")
        sys.exit(1)
    process_encode_folder(target_dir)