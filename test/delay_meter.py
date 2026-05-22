"""
视频推流端到端延迟测量工具（OCR方案） - 优化版
功能：本地毫秒表窗口 + 拉流画面中拍摄的物理秒表（或软件秒表），通过OCR识别时间差，自动统计并导出CSV。
优化：本地表白底黑字，拉流端OCR预处理抗模糊/低分辨率，支持调试图像保存。
增加连续性过滤，避免异常跳变。
"""

import tkinter as tk
from tkinter import ttk, filedialog, messagebox
import threading
import queue
import time
import csv
import os
import re
from datetime import datetime

import pyautogui
import cv2
import numpy as np
import pytesseract
from PIL import Image

# ========================== 配置区 ==========================
pytesseract.pytesseract.tesseract_cmd = r'D:/Tesseract-OCR/tesseract.exe'  # 请修改为您的路径

# ========================== 本地毫秒表窗口（白底黑字） ==========================
class MillisecondClock(tk.Toplevel):
    """独立的本地毫秒表窗口，白底黑字，专为OCR优化"""
    def __init__(self, master=None):
        super().__init__(master)
        self.title('本地毫秒表（可拖动）')
        self.overrideredirect(True)
        self.attributes('-topmost', True)
        self.configure(bg='white')
        
        self.label = tk.Label(
            self,
            font=('Consolas', 72, 'bold'),
            fg='black',
            bg='white'
        )
        self.label.pack(padx=15, pady=10)
        
        self._drag_data = {'x': 0, 'y': 0}
        self.label.bind('<ButtonPress-1>', self._start_drag)
        self.label.bind('<B1-Motion>', self._drag)
        
        self._update_clock()
    
    def _update_clock(self):
        now = datetime.now()
        time_str = now.strftime('%H:%M:%S.') + f'{now.microsecond // 1000:03d}'
        self.label.config(text=time_str)
        self.after(10, self._update_clock)
    
    def _start_drag(self, event):
        self._drag_data['x'] = event.x
        self._drag_data['y'] = event.y
    
    def _drag(self, event):
        x = self.winfo_x() + (event.x - self._drag_data['x'])
        y = self.winfo_y() + (event.y - self._drag_data['y'])
        self.geometry(f'+{x}+{y}')


# ========================== 区域选择器 ==========================
class RegionSelector:
    def __init__(self, master):
        self.master = master
        self.top = tk.Toplevel(master)
        self.top.attributes('-fullscreen', True, '-alpha', 0.3)
        self.top.configure(bg='gray')
        self.top.attributes('-topmost', True)
        self.canvas = tk.Canvas(self.top, cursor='cross', bg='gray')
        self.canvas.pack(fill=tk.BOTH, expand=True)
        self.rect = None
        self.start_x = self.start_y = 0
        self.result = None
        self.canvas.bind('<ButtonPress-1>', self._on_press)
        self.canvas.bind('<B1-Motion>', self._on_drag)
        self.canvas.bind('<ButtonRelease-1>', self._on_release)
        self.top.bind('<Escape>', lambda e: self._cancel())
        self.top.grab_set()
    
    def select(self):
        self.master.wait_window(self.top)
        return self.result
    
    def _on_press(self, event):
        self.start_x, self.start_y = event.x_root, event.y_root
        x1 = self.canvas.canvasx(event.x)
        y1 = self.canvas.canvasy(event.y)
        self.rect = self.canvas.create_rectangle(x1, y1, x1, y1, outline='red', width=3)
    
    def _on_drag(self, event):
        x1, y1 = self.start_x, self.start_y
        x2, y2 = event.x_root, event.y_root
        offset_x = self.top.winfo_x()
        offset_y = self.top.winfo_y()
        self.canvas.coords(self.rect,
                           x1 - offset_x, y1 - offset_y,
                           x2 - offset_x, y2 - offset_y)
    
    def _on_release(self, event):
        x1, y1 = self.start_x, self.start_y
        x2, y2 = event.x_root, event.y_root
        x, y = min(x1, x2), min(y1, y2)
        w, h = abs(x2 - x1), abs(y2 - y1)
        if w > 5 and h > 5:
            self.result = (x, y, w, h)
        self.top.destroy()
    
    def _cancel(self):
        self.top.destroy()


# ========================== 倾斜校正（基于霍夫直线） ==========================
def correct_skew(image):
    """
    检测图像中最长的水平/近水平直线，计算倾斜角度并旋转校正。
    输入：二值图像（白字黑底）
    返回：校正后的二值图像
    """
    edges = cv2.Canny(image, 50, 150, apertureSize=3)
    lines = cv2.HoughLines(edges, 1, np.pi/180, threshold=100)
    
    if lines is None:
        return image
    
    angles = []
    for line in lines:
        rho, theta = line[0]
        angle = np.degrees(theta) - 90
        if abs(angle) < 10:
            angles.append(angle)
    
    if not angles:
        return image
    
    median_angle = np.median(angles)
    h, w = image.shape
    center = (w // 2, h // 2)
    M = cv2.getRotationMatrix2D(center, median_angle, 1.0)
    rotated = cv2.warpAffine(image, M, (w, h), flags=cv2.INTER_CUBIC, borderMode=cv2.BORDER_REPLICATE)
    return rotated


# ========================== OCR识别引擎（本地/远程分离处理） ==========================
def ocr_time_from_region(region, save_debug=False, debug_dir="delay_meter", is_local=False, img_cv=None):
    """
    截取屏幕指定区域，通过预处理和Tesseract识别时间字符串。
    如果 img_cv 不为 None，则直接从中裁剪区域（img_cv 为 BGR 格式的全屏图像）
    """
    if img_cv is None:
        img = pyautogui.screenshot(region=region)
        img_cv = cv2.cvtColor(np.array(img), cv2.COLOR_RGB2BGR)
    else:
        x, y, w, h = region
        img_cv = img_cv[y:y+h, x:x+w]
    
    # 下面的灰度、预处理、OCR 等代码完全保持不变
    gray = cv2.cvtColor(img_cv, cv2.COLOR_BGR2GRAY)
    gray = cv2.GaussianBlur(gray, (3, 3), 0)
    
    if is_local:
        _, thresh = cv2.threshold(gray, 0, 255, cv2.THRESH_BINARY + cv2.THRESH_OTSU)
        thresh = cv2.bitwise_not(thresh)
        kernel_close = cv2.getStructuringElement(cv2.MORPH_RECT, (2, 2))
        closed = cv2.morphologyEx(thresh, cv2.MORPH_CLOSE, kernel_close, iterations=1)
        processed = closed
    else:
        kernel_sharpen = np.array([[-1,-1,-1], [-1,9,-1], [-1,-1,-1]])
        sharp = cv2.filter2D(gray, -1, kernel_sharpen)
        thresh = cv2.adaptiveThreshold(sharp, 255, cv2.ADAPTIVE_THRESH_GAUSSIAN_C,
                                       cv2.THRESH_BINARY_INV, 15, 8)
        thresh = correct_skew(thresh)
        kernel_close = cv2.getStructuringElement(cv2.MORPH_RECT, (2, 2))
        closed = cv2.morphologyEx(thresh, cv2.MORPH_CLOSE, kernel_close, iterations=1)
        kernel_open = cv2.getStructuringElement(cv2.MORPH_RECT, (2, 2))
        opened = cv2.morphologyEx(closed, cv2.MORPH_OPEN, kernel_open, iterations=1)
        opened = cv2.medianBlur(opened, 3)
        kernel_erode = cv2.getStructuringElement(cv2.MORPH_RECT, (2, 2))
        eroded = cv2.erode(opened, kernel_erode, iterations=1)
        # 第二次开运算，去除可能残留的黑色噪点
        kernel_open2 = cv2.getStructuringElement(cv2.MORPH_RECT, (2, 2))
        opened2 = cv2.morphologyEx(eroded, cv2.MORPH_OPEN, kernel_open2, iterations=1)
        processed = eroded
    
    h, w = processed.shape
    scale = 4
    enlarged = cv2.resize(processed, (w*scale, h*scale), interpolation=cv2.INTER_CUBIC)
    
    if save_debug:
        os.makedirs(debug_dir, exist_ok=True)
        timestamp = datetime.now().strftime('%Y%m%d_%H%M%S_%f')[:-3]
        prefix = "local" if is_local else "remote"
        cv2.imwrite(os.path.join(debug_dir, f'{prefix}_ocr_debug_{timestamp}.png'), enlarged)
    
    custom_config = r'--oem 3 --psm 7 -c tessedit_char_whitelist=0123456789:.'
    text = pytesseract.image_to_string(enlarged, config=custom_config).strip()
    digits = re.sub(r'[^0-9]', '', text)
    
    # 尝试9位数字解析
    if len(digits) == 9:
        hh = int(digits[0:2])
        mm = int(digits[2:4])
        ss = int(digits[4:6])
        ms = int(digits[6:9])
        if 0 <= hh <= 23 and 0 <= mm <= 59 and 0 <= ss <= 59 and 0 <= ms <= 999:
            ms_total = hh * 3600000 + mm * 60000 + ss * 1000 + ms
            if 0 <= ms_total <= 86400000:
                return ms_total
    else:
        # 对于远程区域，如果长度小于9且大于0，尝试左补零到9位
        if not is_local and 0 < len(digits) < 9:
            digits_padded = digits.zfill(9)
            hh = int(digits_padded[0:2])
            mm = int(digits_padded[2:4])
            ss = int(digits_padded[4:6])
            ms = int(digits_padded[6:9])
            if 0 <= hh <= 23 and 0 <= mm <= 59 and 0 <= ss <= 59 and 0 <= ms <= 999:
                ms_total = hh * 3600000 + mm * 60000 + ss * 1000 + ms
                if 0 <= ms_total <= 86400000:
                    return ms_total
    
    # 降级：尝试冒号点格式
    match = re.search(r'(\d{1,2}:)?(\d{1,2}:)?(\d{2})[\.:](\d{1,3})', text)
    if match:
        hh, mm, ss, ms = match.groups()
        hour = int(hh[:-1]) if hh else 0
        minute = int(mm[:-1]) if mm else 0
        sec = int(ss)
        millisec = int(ms.ljust(3, '0')[:3])
        ms_total = hour * 3600000 + minute * 60000 + sec * 1000 + millisec
        if 0 <= ms_total <= 86400000:
            return ms_total
    
    return None


# ========================== 测量引擎（线程，支持调试开关和连续性过滤） ==========================
class DelayMeter:
    def __init__(self, local_region, remote_region, interval_ms, duration_min, csv_path, result_queue, debug_mode=False):
        self.local_region = local_region
        self.remote_region = remote_region
        self.interval = interval_ms / 1000.0
        self.duration = duration_min * 60.0
        self.csv_path = csv_path
        self.queue = result_queue
        self.debug_mode = debug_mode
        self._stop_flag = False
        self.last_valid_local = None
        self.last_valid_remote = None
    
    def stop(self):
        self._stop_flag = True
    
    def run(self):
        try:
            with open(self.csv_path, 'w', newline='', encoding='utf-8-sig') as f:
                writer = csv.writer(f)
                writer.writerow(['timestamp', 'delay_ms', 'local_raw', 'remote_raw'])
                
                start_time = time.time()
                while not self._stop_flag:
                    if self.duration > 0 and (time.time() - start_time) >= self.duration:
                        break
                    
                    loop_start = time.time()

                    
                    # 先计算两个区域的最小包围矩形，减少截图大小
                    x1, y1, w1, h1 = self.local_region
                    x2, y2, w2, h2 = self.remote_region
                    min_x = min(x1, x2)
                    min_y = min(y1, y2)
                    max_x = max(x1 + w1, x2 + w2)
                    max_y = max(y1 + h1, y2 + h2)
                    combined_rect = (min_x, min_y, max_x - min_x, max_y - min_y)
                    # 一次性截图
                    combined_img = pyautogui.screenshot(region=combined_rect)
                    combined_img_cv = cv2.cvtColor(np.array(combined_img), cv2.COLOR_RGB2BGR)
                    # 分别从全图中裁剪出本地和远程区域（注意坐标偏移）
                    local_region_offset = (self.local_region[0] - min_x, self.local_region[1] - min_y, self.local_region[2], self.local_region[3])
                    remote_region_offset = (self.remote_region[0] - min_x, self.remote_region[1] - min_y, self.remote_region[2], self.remote_region[3])
                    ms_local = ocr_time_from_region(local_region_offset, save_debug=False, is_local=True, img_cv=combined_img_cv)
                    ms_remote = ocr_time_from_region(remote_region_offset, save_debug=self.debug_mode, is_local=False, img_cv=combined_img_cv)
                    
                    # 连续性检查
                    valid_local = True
                    valid_remote = True
                    if ms_local is not None and self.last_valid_local is not None:
                        diff = ms_local - self.last_valid_local
                        if diff < -43200000:  # 跨天
                            diff += 86400000
                        if not (0 <= diff <= 5000):  # 允许最大跳变5秒
                            valid_local = False
                    if ms_remote is not None and self.last_valid_remote is not None:
                        diff = ms_remote - self.last_valid_remote
                        if diff < -43200000:
                            diff += 86400000
                        if not (0 <= diff <= 5000):
                            valid_remote = False
                    
                    # 只有当两个值都有效且连续性通过时，才记录
                    if ms_local is not None and ms_remote is not None and valid_local and valid_remote:
                        # 更新有效历史
                        self.last_valid_local = ms_local
                        self.last_valid_remote = ms_remote
                        
                        diff = ms_local - ms_remote
                        # 跨天修正（仅当确实跨过午夜，差值小于 -12 小时）
                        if diff < -43200000:
                            diff += 86400000
                        # 负延迟或过大的正延迟（>10秒）均为异常，丢弃
                        if diff < 0 or diff > 10000:
                            self.queue.put(None)
                            continue   # 跳过本次循环，不记录到 CSV
                        
                        writer.writerow([
                            datetime.now().strftime('%Y-%m-%d %H:%M:%S.%f')[:-3],
                            f'{diff:.0f}',
                            str(ms_local),
                            str(ms_remote)
                        ])
                        f.flush()
                        self.queue.put(diff)
                    else:
                        # 识别失败或连续性异常，不更新历史，仅记录失败
                        self.queue.put(None)
                    
                    elapsed = time.time() - loop_start
                    sleep_time = max(0, self.interval - elapsed)
                    time.sleep(sleep_time)
                
                self.queue.put('DONE')
        except Exception as e:
            self.queue.put(('ERROR', str(e)))


# ========================== 主设置窗口（同前，无需修改） ==========================
class SettingsWindow(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title('视频延迟测量工具（OCR优化版 - 拍摄秒表）')
        self.geometry('520x520')
        self.resizable(False, False)
        
        self.local_region = None
        self.remote_region = None
        self.clock_window = None
        self.meter = None
        self.measure_thread = None
        self.running = False
        self.queue = queue.Queue()
        self.samples = []
        self.fail_count = 0
        
        self._build_ui()
        self.protocol('WM_DELETE_WINDOW', self._on_closing)
        
        self.clock_window = MillisecondClock(self)
        self.clock_window.geometry('+100+100')
        
        self.after(100, self._poll_results)
    
    def _build_ui(self):
        frame_select = ttk.LabelFrame(self, text='1. 框选秒表区域', padding=10)
        frame_select.pack(fill=tk.X, padx=10, pady=5)
        
        ttk.Label(frame_select, text='本地参考秒表区域（程序白底黑字窗口）').grid(row=0, column=0, sticky=tk.W)
        self.btn_local = ttk.Button(frame_select, text='选择区域', command=self._select_local)
        self.btn_local.grid(row=0, column=1, padx=5, pady=2)
        self.lbl_local = ttk.Label(frame_select, text='未选择', foreground='gray')
        self.lbl_local.grid(row=0, column=2, sticky=tk.W)
        
        ttk.Label(frame_select, text='拉流画面中拍摄的秒表区域（物理或外部秒表）').grid(row=1, column=0, sticky=tk.W)
        self.btn_remote = ttk.Button(frame_select, text='选择区域', command=self._select_remote)
        self.btn_remote.grid(row=1, column=1, padx=5, pady=2)
        self.lbl_remote = ttk.Label(frame_select, text='未选择', foreground='gray')
        self.lbl_remote.grid(row=1, column=2, sticky=tk.W)
        
        frame_param = ttk.LabelFrame(self, text='2. 参数设置', padding=10)
        frame_param.pack(fill=tk.X, padx=10, pady=5)
        
        ttk.Label(frame_param, text='识别间隔（毫秒）').grid(row=0, column=0, sticky=tk.W)
        self.interval_var = tk.StringVar(value='500')
        ttk.Entry(frame_param, textvariable=self.interval_var, width=10).grid(row=0, column=1, sticky=tk.W, padx=5)
        
        ttk.Label(frame_param, text='测试总时长（分钟，0=无限）').grid(row=1, column=0, sticky=tk.W)
        self.duration_var = tk.StringVar(value='5')
        ttk.Entry(frame_param, textvariable=self.duration_var, width=10).grid(row=1, column=1, sticky=tk.W, padx=5)
        
        ttk.Label(frame_param, text='CSV保存路径').grid(row=2, column=0, sticky=tk.W)
        self.csv_path_var = tk.StringVar(value='delay_log.csv')
        ttk.Entry(frame_param, textvariable=self.csv_path_var, width=35).grid(row=2, column=1, columnspan=2, sticky=tk.W, padx=5)
        ttk.Button(frame_param, text='浏览', command=self._browse_csv).grid(row=2, column=3)
        
        self.debug_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(frame_param, text='开启调试模式（保存远程区域预处理图像到delay_meter目录）',
                        variable=self.debug_var).grid(row=3, column=0, columnspan=4, sticky=tk.W, pady=5)
        
        frame_ctrl = ttk.Frame(self)
        frame_ctrl.pack(fill=tk.X, padx=10, pady=10)
        self.btn_start = ttk.Button(frame_ctrl, text='开始测量', command=self._start_measurement)
        self.btn_start.pack(side=tk.LEFT, padx=5)
        self.btn_stop = ttk.Button(frame_ctrl, text='停止测量', command=self._stop_measurement, state=tk.DISABLED)
        self.btn_stop.pack(side=tk.LEFT, padx=5)
        
        frame_status = ttk.LabelFrame(self, text='3. 实时状态', padding=10)
        frame_status.pack(fill=tk.BOTH, expand=True, padx=10, pady=5)
        status_grid = ttk.Frame(frame_status)
        status_grid.pack()
        
        ttk.Label(status_grid, text='当前延迟：').grid(row=0, column=0, sticky=tk.W, pady=2)
        self.lbl_current = ttk.Label(status_grid, text='---', font=('Arial', 12, 'bold'))
        self.lbl_current.grid(row=0, column=1, sticky=tk.W, padx=5)
        
        ttk.Label(status_grid, text='平均延迟：').grid(row=1, column=0, sticky=tk.W, pady=2)
        self.lbl_avg = ttk.Label(status_grid, text='---')
        self.lbl_avg.grid(row=1, column=1, sticky=tk.W, padx=5)
        
        ttk.Label(status_grid, text='成功样本：').grid(row=2, column=0, sticky=tk.W, pady=2)
        self.lbl_samples = ttk.Label(status_grid, text='0')
        self.lbl_samples.grid(row=2, column=1, sticky=tk.W, padx=5)
        
        ttk.Label(status_grid, text='识别失败：').grid(row=3, column=0, sticky=tk.W, pady=2)
        self.lbl_fail = ttk.Label(status_grid, text='0')
        self.lbl_fail.grid(row=3, column=1, sticky=tk.W, padx=5)
        
        self.progress = ttk.Progressbar(self, mode='indeterminate')
    
    def _select_local(self):
        self.attributes('-alpha', 0.0)
        self.update()
        time.sleep(0.2)
        selector = RegionSelector(self)
        result = selector.select()
        self.attributes('-alpha', 1.0)
        if result:
            self.local_region = result
            x,y,w,h = result
            self.lbl_local.config(text=f'({x},{y}) {w}x{h}', foreground='black')
        else:
            self.lbl_local.config(text='已取消', foreground='gray')
    
    def _select_remote(self):
        self.attributes('-alpha', 0.0)
        self.update()
        time.sleep(0.2)
        selector = RegionSelector(self)
        result = selector.select()
        self.attributes('-alpha', 1.0)
        if result:
            self.remote_region = result
            x,y,w,h = result
            self.lbl_remote.config(text=f'({x},{y}) {w}x{h}', foreground='black')
        else:
            self.lbl_remote.config(text='已取消', foreground='gray')
    
    def _browse_csv(self):
        path = filedialog.asksaveasfilename(defaultextension='.csv', filetypes=[('CSV文件', '*.csv')])
        if path:
            self.csv_path_var.set(path)
    
    def _start_measurement(self):
        if not self.local_region or not self.remote_region:
            messagebox.showwarning('缺少区域', '请先框选两个秒表区域。')
            return
        try:
            interval_ms = int(self.interval_var.get())
            duration_min = float(self.duration_var.get())
        except ValueError:
            messagebox.showerror('参数错误', '间隔和时长必须为数字。')
            return
        
        csv_path = self.csv_path_var.get()
        debug_mode = self.debug_var.get()
        
        self.samples = []
        self.fail_count = 0
        self.queue = queue.Queue()
        
        self.meter = DelayMeter(
            self.local_region, self.remote_region,
            interval_ms, duration_min, csv_path, self.queue, debug_mode
        )
        self.measure_thread = threading.Thread(target=self.meter.run, daemon=True)
        self.measure_thread.start()
        
        self.running = True
        self.btn_start.config(state=tk.DISABLED)
        self.btn_stop.config(state=tk.NORMAL)
        self.btn_local.config(state=tk.DISABLED)
        self.btn_remote.config(state=tk.DISABLED)
        self.progress.pack(fill=tk.X, padx=10, pady=5)
        self.progress.start(10)
        self.lbl_current.config(text='等待首次结果...')
    
    def _stop_measurement(self):
        if self.meter:
            self.meter.stop()
        self.running = False
        self.btn_start.config(state=tk.NORMAL)
        self.btn_stop.config(state=tk.DISABLED)
        self.btn_local.config(state=tk.NORMAL)
        self.btn_remote.config(state=tk.NORMAL)
        self.progress.stop()
        self.progress.pack_forget()
        self.lbl_current.config(text='已停止')
    
    def _poll_results(self):
        try:
            while True:
                item = self.queue.get_nowait()
                if isinstance(item, str) and item == 'DONE':
                    self._stop_measurement()
                    messagebox.showinfo('完成', '测量已完成，结果已保存。')
                elif isinstance(item, (int, float)):
                    self.samples.append(item)
                    self.lbl_current.config(text=f'{item:.0f} ms')
                    if self.samples:
                        avg = sum(self.samples) / len(self.samples)
                        self.lbl_avg.config(text=f'{avg:.1f} ms')
                    self.lbl_samples.config(text=str(len(self.samples)))
                elif item is None:
                    self.fail_count += 1
                    self.lbl_fail.config(text=str(self.fail_count))
                elif isinstance(item, tuple) and item[0] == 'ERROR':
                    messagebox.showerror('错误', item[1])
                    self._stop_measurement()
        except queue.Empty:
            pass
        self.after(100, self._poll_results)
    
    def _on_closing(self):
        if self.running:
            self._stop_measurement()
        if self.clock_window:
            self.clock_window.destroy()
        self.destroy()


if __name__ == '__main__':
    app = SettingsWindow()
    app.mainloop()