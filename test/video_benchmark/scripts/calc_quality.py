#!/usr/bin/env python3
"""
PC 端质量计算脚本（PSNR从stderr, SSIM从日志, 均验证通过）
用法: python3 calc_quality.py encode_xxx
"""
import os, sys, csv, subprocess, json, tempfile, re

RAW_FILE = "raw.yuv"
WIDTH, HEIGHT = 640, 480
PIX_FMT_RAW = "yuyv422"
FRAMERATE = 25                # 必须与采集/编码一致

def compute_vmaf(encoded_file, tmp_dir):
    """使用 libvmaf 滤镜计算 VMAF"""
    log_file = os.path.join(tmp_dir, "vmaf.json")
    filter_complex = (
        f"[0:v]format=yuv420p,setpts=PTS-STARTPTS[ref];"
        f"[1:v]setpts=PTS-STARTPTS[enc];"
        f"[ref][enc]libvmaf="
        f"log_fmt=json:"
        f"log_path={log_file}:"
        f"shortest=1"
    )
    cmd = [
        "ffmpeg", "-hide_banner", "-y",
        "-f", "rawvideo", "-pix_fmt", PIX_FMT_RAW,
        "-video_size", f"{WIDTH}x{HEIGHT}",
        "-framerate", str(FRAMERATE),
        "-i", RAW_FILE,
        "-i", encoded_file,
        "-lavfi", filter_complex,
        "-f", "null", "-"
    ]
    try:
        subprocess.run(cmd, capture_output=True, timeout=300)
    except Exception as e:
        print(f"  VMAF 失败: {e}")
        return None
    if not os.path.exists(log_file):
        print(f"  警告: VMAF 日志未生成")
        return None
    try:
        with open(log_file) as f:
            data = json.load(f)
        return data['pooled_metrics']['vmaf']['mean']
    except Exception as e:
        print(f"  VMAF JSON 解析失败: {e}")
        return None

def compute_psnr_ssim(encoded_file, tmp_dir):
    """计算 PSNR (从stderr) 和 SSIM (从日志文件)"""
    base_filter = (
        f"[0:v]format=yuv420p,setpts=PTS-STARTPTS[ref];"
        f"[1:v]setpts=PTS-STARTPTS[enc];"
    )

    # ---- PSNR ----
    psnr_cmd = [
        "ffmpeg", "-hide_banner", "-y",
        "-f", "rawvideo", "-pix_fmt", PIX_FMT_RAW,
        "-video_size", f"{WIDTH}x{HEIGHT}",
        "-framerate", str(FRAMERATE),
        "-i", RAW_FILE,
        "-i", encoded_file,
        "-lavfi", base_filter + "[ref][enc]psnr=shortest=1",
        "-f", "null", "-"
    ]
    proc_psnr = subprocess.run(psnr_cmd, capture_output=True, text=True, timeout=120)
    psnr_data = extract_psnr_from_stderr(proc_psnr.stderr)
    if not psnr_data:
        print("  [调试] PSNR 解析失败，最后 300 字符 stderr:")
        print(proc_psnr.stderr[-300:])

    # ---- SSIM ----
    ssim_log = os.path.join(tmp_dir, "ssim.log")
    ssim_cmd = [
        "ffmpeg", "-hide_banner", "-y",
        "-f", "rawvideo", "-pix_fmt", PIX_FMT_RAW,
        "-video_size", f"{WIDTH}x{HEIGHT}",
        "-framerate", str(FRAMERATE),
        "-i", RAW_FILE,
        "-i", encoded_file,
        "-lavfi", base_filter + f"[ref][enc]ssim=stats_file={ssim_log}:shortest=1",
        "-f", "null", "-"
    ]
    subprocess.run(ssim_cmd, capture_output=True, timeout=120)
    ssim_data = extract_ssim_from_log(ssim_log)

    metrics = {}
    metrics.update(psnr_data)
    metrics.update(ssim_data)
    return metrics

def extract_psnr_from_stderr(stderr_text):
    """从 stderr 提取最终 PSNR 平均值"""
    last_psnr_line = ""
    for line in stderr_text.splitlines():
        if "PSNR" in line and "average:" in line:
            last_psnr_line = line
    if last_psnr_line:
        m = re.search(r'y:(\S+)\s+u:(\S+)\s+v:(\S+)\s+average:(\S+)', last_psnr_line)
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

def compute_all_metrics(encoded_file, tmp_dir):
    vmaf = compute_vmaf(encoded_file, tmp_dir)
    psnr_ssim = compute_psnr_ssim(encoded_file, tmp_dir)
    if vmaf is not None:
        psnr_ssim['vmaf'] = vmaf
    return psnr_ssim

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
        print("CSV 缺少 'group' 列"); return
    new_cols = ['psnr_y','psnr_u','psnr_v','psnr_avg',
                'ssim_y','ssim_u','ssim_v','ssim_all','vmaf']
    for col in new_cols:
        if col not in header:
            header.append(col)

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
                print(f"组 {group_id}: 找不到 .h264, 跳过")
                continue
            print(f"计算组 {group_id}: {os.path.basename(encoded_file)}")
            all_metrics = compute_all_metrics(encoded_file, tmp_dir)
            while len(row) < len(header):
                row.append("")
            for col, val in all_metrics.items():
                if col in header:
                    row[header.index(col)] = val

    with open(result_csv_path, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerows(rows)
    print(f"更新完成: {result_csv_path}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("用法: python3 calc_quality.py encode_xxx")
        sys.exit(1)
    target_dir = sys.argv[1]
    os.chdir(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    if not os.path.exists(RAW_FILE):
        print(f"错误: 根目录缺少 {RAW_FILE}")
        sys.exit(1)
    process_encode_folder(target_dir)