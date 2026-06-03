#!/usr/bin/env python3
import argparse
import json
import requests
import sys

def main():
    parser = argparse.ArgumentParser(description='发送远程指令到设备')
    parser.add_argument('-u', '--url', required=True, help='服务器地址，如 http://192.168.1.3:5000')
    parser.add_argument('-d', '--device-id', type=int, required=True, help='设备ID')
    parser.add_argument('--capture', type=int, choices=[0, 1], help='采集开关：0=停止，1=开始')
    parser.add_argument('--crf', type=int, help='CRF值 (0-51)')
    parser.add_argument('--bitrate', type=int, help='码率 (kbps)')
    parser.add_argument('--gop', type=int, help='GOP大小')

    args = parser.parse_args()

    # 构造指令列表
    commands = []
    if args.capture is not None:
        commands.append({
            "cmd": "enable_capture",
            "params": {"enable": bool(args.capture)}
        })
    if args.crf is not None:
        commands.append({
            "cmd": "set_crf",
            "params": {"crf": args.crf}
        })
    if args.bitrate is not None:
        commands.append({
            "cmd": "set_max_bitrate",
            "params": {"bitrate": args.bitrate}
        })
    if args.gop is not None:
        commands.append({
            "cmd": "set_gop",
            "params": {"gop": args.gop}
        })

    if not commands:
        print("错误：至少需要指定一个参数", file=sys.stderr)
        sys.exit(1)

    # 构造完整JSON
    payload = {"commands": commands}
    url = f"{args.url}/command?device_id={args.device_id}"

    try:
        resp = requests.post(url, json=payload, timeout=5)
        if resp.status_code == 200:
            print("指令已发送，服务器响应：", resp.json())
        else:
            print(f"发送失败，HTTP {resp.status_code}: {resp.text}", file=sys.stderr)
            sys.exit(1)
    except Exception as e:
        print(f"网络错误: {e}", file=sys.stderr)
        sys.exit(1)

if __name__ == '__main__':
    main()