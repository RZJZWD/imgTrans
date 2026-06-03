from flask import Flask, request, jsonify
from collections import defaultdict
import threading
import argparse
import subprocess
import sys
import logging

app = Flask(__name__)

# 存储每个设备的指令队列（每个元素是一个指令数组，而不是单条指令）
queues = defaultdict(list)
queues_lock = threading.Lock()

# 外部程序进程对象
external_proc = None

@app.route('/command', methods=['POST'])
def client_push_command():
    """
    客户端推送指令入队
    请求体 JSON 格式：{"commands": [指令对象1, 指令对象2, ...]}
    将整个 commands 数组作为一个整体存入队列
    """
    data = request.get_json()
    if not data or 'commands' not in data or not isinstance(data['commands'], list):
        return jsonify({"error": "Invalid format, expected {'commands': [...]}"}), 400

    device_id = request.args.get('device_id')
    if not device_id:
        return jsonify({"error": "Missing device_id"}), 400
    device_id = int(device_id)

    with queues_lock:
        # 将整个指令数组追加到队列尾部
        queues[device_id].append(data['commands'])

    return jsonify({"status": "ok", "queued": len(data['commands'])}), 200

@app.route('/command', methods=['GET'])
def device_pop_command():
    """
    设备端轮询，从队列头部取出一个指令数组，并立即清空该设备的所有待处理指令
    （因为一次返回全部，无需保留）
    返回格式：{"commands": [...]}，若无指令则返回 {"commands": []}
    """
    device_id = request.args.get('device_id')
    if not device_id:
        return jsonify({"error": "Missing device_id"}), 400
    device_id = int(device_id)

    with queues_lock:
        q = queues.get(device_id, [])
        if q:
            # 取出队列头部的指令数组（整个数组）
            commands = q.pop(0)
            # 注意：这里没有清空整个队列，只是取出了第一条。
            # 如果需要一次性取出所有待处理的指令，可以改为：
            # commands = []
            # while q:
            #     commands.extend(q.pop(0))
            # 但通常客户端不会同时发送大量数组，取第一条即可。
            return jsonify({"commands": commands}), 200
        else:
            return jsonify({"commands": []}), 200

def start_external_app(cmd):
    """启动外部程序（如 MediaMTX）并打印信息"""
    global external_proc
    try:
        if isinstance(cmd, str):
            args = cmd.split()
        else:
            args = cmd
        external_proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        print(f"外部程序已启动: {' '.join(args)} (PID: {external_proc.pid})")
        # 启动线程读取输出，避免阻塞
        def read_output(pipe, prefix):
            for line in iter(pipe.readline, b''):
                print(f"[{prefix}] {line.decode().rstrip()}")
        if external_proc.stdout:
            threading.Thread(target=read_output, args=(external_proc.stdout, "STDOUT"), daemon=True).start()
        if external_proc.stderr:
            threading.Thread(target=read_output, args=(external_proc.stderr, "STDERR"), daemon=True).start()
    except Exception as e:
        print(f"启动外部程序失败: {e}", file=sys.stderr)

if __name__ == '__main__':
    # 添加命令行参数解析，允许指定要启动的外部程序
    parser = argparse.ArgumentParser(description='远程指令服务器')
    parser.add_argument('--external-cmd', help='要启动的外部程序及其参数，例如 "./mediamtx"')
    args = parser.parse_args()

    # 如果提供了外部程序路径，则启动它
    if args.external_cmd:
        start_external_app(args.external_cmd)

    # 屏蔽 Flask 默认的访问日志，避免板卡轮询的 GET 请求输出到控制台
    log = logging.getLogger('werkzeug')
    log.setLevel(logging.ERROR)  # 只显示错误，不显示访问日志

    print("服务器启动，监听 0.0.0.0:5000")
    app.run(host='0.0.0.0', port=5000, debug=False)  # 关闭 debug 模式，避免额外日志