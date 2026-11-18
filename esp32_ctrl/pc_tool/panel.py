"""Gripp Pad 燒錄控制面板

選板子、選序列埠,按一下就編譯 / 燒錄 / 看序列埠訊息。
板子清單直接讀上一層的 platformio.ini,所以在那邊加新板子,這裡就會多一個選項。

只用到 Python 內建的東西(tkinter),不用另外 pip install。
"""
import json
import os
import re
import socket
import subprocess
import sys
import threading
import tkinter as tk
import webbrowser
from pathlib import Path
from tkinter import messagebox, ttk

PROJECT = Path(__file__).resolve().parent.parent
INI = PROJECT / "platformio.ini"
UDP_PORT = 4444
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)
NEW_CONSOLE = getattr(subprocess, "CREATE_NEW_CONSOLE", 0)
# PlatformIO 會印進度條的特殊字元(░ 之類)。繁中 Windows 預設是 cp950,印不出來就會當掉,
# 所以啟動它的時候一律強制用 UTF-8。
CHILD_ENV = {**os.environ, "PYTHONIOENCODING": "utf-8", "PYTHONUTF8": "1"}

BG, CARD, GOLD, TEXT, DIM = "#15161b", "#22242c", "#ffc400", "#f2f2f2", "#9aa0ad"


def find_pio():
    """PlatformIO 裝在固定位置,不用靠 PATH。"""
    exe = "pio.exe" if os.name == "nt" else "pio"
    for folder in ("Scripts", "bin"):
        p = Path.home() / ".platformio" / "penv" / folder / exe
        if p.exists():
            return str(p)
    return "pio"


def read_boards():
    """回傳 [(環境名稱, 說明)],說明取自 [env:xxx] 上一行的「; 說明:」。"""
    boards, note = [], ""
    for line in INI.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        m = re.match(r"^;\s*說明[::]\s*(.+)$", line)
        if m:
            note = m.group(1)
            continue
        m = re.match(r"^\[env:([^\]]+)\]$", line)
        if m:
            boards.append((m.group(1), note or m.group(1)))
        if line and not line.startswith(";"):
            note = ""
    return boards


def default_board():
    m = re.search(r"^default_envs\s*=\s*(\S+)", INI.read_text(encoding="utf-8"), re.M)
    return m.group(1) if m else None


class Panel(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Gripp Pad 燒錄控制面板")
        self.configure(bg=BG)
        self.geometry("860x600")
        self.minsize(720, 480)

        self.pio = find_pio()
        self.boards = read_boards()
        self.job = None          # 正在跑的 編譯 / 燒錄
        self.monitor = None      # 另外開的序列埠監控視窗
        self.found = {}          # ip -> 板子廣播的資訊

        self._build_ui()
        self.refresh_ports()
        threading.Thread(target=self._listen_boards, daemon=True).start()
        self.protocol("WM_DELETE_WINDOW", self._on_close)

    # ---- 畫面 ----------------------------------------------------------
    def _build_ui(self):
        style = ttk.Style(self)
        style.theme_use("clam")
        style.configure("TCombobox", fieldbackground=CARD, background=CARD, foreground=TEXT, arrowcolor=TEXT)
        style.map("TCombobox", fieldbackground=[("readonly", CARD)], foreground=[("readonly", TEXT)])

        top = tk.Frame(self, bg=BG)
        top.pack(fill="x", padx=16, pady=(14, 6))
        tk.Label(top, text="Gripp Pad", bg=BG, fg=GOLD, font=("Segoe UI", 18, "bold")).pack(side="left")
        tk.Label(top, text="  ESP32 燒錄控制面板", bg=BG, fg=DIM, font=("Microsoft JhengHei UI", 11)).pack(side="left", pady=(6, 0))

        row = tk.Frame(self, bg=BG)
        row.pack(fill="x", padx=16, pady=4)
        tk.Label(row, text="板子", bg=BG, fg=TEXT, width=6, anchor="w").pack(side="left")
        self.board_box = ttk.Combobox(row, state="readonly", width=46, values=[f"{d}   [{n}]" for n, d in self.boards])
        names = [n for n, _ in self.boards]
        want = default_board()
        self.board_box.current(names.index(want) if want in names else 0)
        self.board_box.pack(side="left", padx=(0, 16))

        tk.Label(row, text="序列埠", bg=BG, fg=TEXT, width=6, anchor="w").pack(side="left")
        self.port_box = ttk.Combobox(row, state="readonly", width=26)
        self.port_box.pack(side="left")
        self._button(row, "重新整理", self.refresh_ports, small=True).pack(side="left", padx=8)

        bar = tk.Frame(self, bg=BG)
        bar.pack(fill="x", padx=16, pady=10)
        self._button(bar, "燒錄", lambda: self.run_pio("upload"), primary=True).pack(side="left")
        self._button(bar, "燒錄後看訊息", lambda: self.run_pio("upload", then_monitor=True)).pack(side="left", padx=8)
        self._button(bar, "只編譯", lambda: self.run_pio("build")).pack(side="left")
        self._button(bar, "序列埠監控", self.open_monitor).pack(side="left", padx=8)
        self._button(bar, "整顆清空", self.erase).pack(side="left")
        self._button(bar, "停止", self.stop).pack(side="right")

        info = tk.Frame(self, bg=CARD)
        info.pack(fill="x", padx=16, pady=(0, 8))
        self.found_label = tk.Label(info, text="還沒在網路上看到板子(板子連上 WiFi 後會自己出現)", bg=CARD, fg=DIM, anchor="w", padx=10, pady=8)
        self.found_label.pack(side="left", fill="x", expand=True)
        self.web_btn = self._button(info, "打開板子網頁", self.open_web, small=True)
        self.web_btn.pack(side="right", padx=8, pady=6)

        self.log = tk.Text(self, bg="#0c0d10", fg="#d6d9e0", insertbackground=TEXT, relief="flat", font=("Consolas", 10), wrap="word")
        self.log.pack(fill="both", expand=True, padx=16, pady=(0, 14))
        self.log.tag_config("ok", foreground="#7ee787")
        self.log.tag_config("err", foreground="#ff7b72")
        self.log.tag_config("cmd", foreground=GOLD)
        self.say("燒不進去的時候:按住板子上的 BOOT,點一下 RESET,放開 BOOT,再按一次「燒錄」。\n", "cmd")

    def _button(self, parent, text, cmd, primary=False, small=False):
        return tk.Button(parent, text=text, command=cmd, relief="flat", cursor="hand2",
                         bg=GOLD if primary else CARD, fg="#1a1300" if primary else TEXT,
                         activebackground="#ffd84d" if primary else "#30333d", activeforeground="#1a1300" if primary else TEXT,
                         font=("Microsoft JhengHei UI", 9 if small else 11, "bold" if primary else "normal"),
                         padx=10 if small else 16, pady=2 if small else 6, bd=0)

    def say(self, text, tag=None):
        self.log.insert("end", text, tag)
        self.log.see("end")

    # ---- 板子 / 序列埠 --------------------------------------------------
    def board(self):
        return self.boards[self.board_box.current()][0]

    def port(self):
        value = self.port_box.get()
        return value.split()[0] if value and not value.startswith("(") else None

    def refresh_ports(self):
        def work():
            ports = []
            try:
                out = subprocess.run([self.pio, "device", "list", "--json-output"], capture_output=True, text=True,
                                     timeout=20, creationflags=NO_WINDOW, env=CHILD_ENV, encoding="utf-8", errors="replace").stdout
                for p in json.loads(out or "[]"):
                    ports.append(f"{p.get('port', '')}  {p.get('description', '')}".strip())
            except Exception as e:  # 找不到 pio、逾時…
                self.after(0, self.say, f"讀不到序列埠清單:{e}\n", "err")
            self.after(0, self._set_ports, ports)
        threading.Thread(target=work, daemon=True).start()

    def _set_ports(self, ports):
        keep = self.port_box.get()
        self.port_box["values"] = ports or ["(沒找到,自動偵測)"]
        if keep in ports:
            self.port_box.set(keep)
        else:
            # 優先選看起來像 ESP32 的(USB JTAG / CH340 / CP210x)
            pick = next((p for p in ports if re.search(r"USB|CH34|CP21|JTAG|Serial", p, re.I) and "Bluetooth" not in p), None)
            self.port_box.set(pick or (ports[0] if ports else "(沒找到,自動偵測)"))

    # ---- 編譯 / 燒錄 ----------------------------------------------------
    def run_pio(self, action, then_monitor=False):
        if self.job and self.job.poll() is None:
            self.say("上一個動作還在跑,先等它結束或按「停止」。\n", "err")
            return
        self.close_monitor()  # 監控視窗會佔住序列埠,燒錄前先關掉
        cmd = [self.pio, "run", "-d", str(PROJECT), "-e", self.board()]
        if action in ("upload", "erase"):
            cmd += ["-t", action]
            if self.port():
                cmd += ["--upload-port", self.port()]
        self.say("\n> " + " ".join(cmd) + "\n", "cmd")

        def work():
            try:
                self.job = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                            encoding="utf-8", errors="replace", creationflags=NO_WINDOW, env=CHILD_ENV)
            except FileNotFoundError:
                self.after(0, self.say, "找不到 PlatformIO。請先在 VS Code 裝 PlatformIO(pioarduino)擴充套件。\n", "err")
                return
            for line in self.job.stdout:
                self.after(0, self.say, line)
            code = self.job.wait()
            ok = code == 0
            self.after(0, self.say, ("完成 ✔\n" if ok else f"失敗(代碼 {code}),往上看紅字或最後幾行。\n"), "ok" if ok else "err")
            if ok and then_monitor:
                self.after(800, self.open_monitor)
        threading.Thread(target=work, daemon=True).start()

    def erase(self):
        if messagebox.askyesno("整顆清空", "會把板子裡的程式和 WiFi 設定全部清掉,確定嗎?"):
            self.run_pio("erase")

    def stop(self):
        if self.job and self.job.poll() is None:
            self._kill(self.job)
            self.say("已停止。\n", "err")

    # ---- 序列埠監控(另開一個視窗,比較穩) ------------------------------
    def open_monitor(self):
        self.close_monitor()
        cmd = [self.pio, "device", "monitor", "-d", str(PROJECT), "-e", self.board()]
        if self.port():
            cmd += ["-p", self.port()]
        try:
            self.monitor = subprocess.Popen(cmd, creationflags=NEW_CONSOLE, env=CHILD_ENV)
            self.say("已另開序列埠監控視窗(燒錄時會自動關掉)。\n", "cmd")
        except FileNotFoundError:
            self.say("找不到 PlatformIO。\n", "err")

    def close_monitor(self):
        if self.monitor and self.monitor.poll() is None:
            self._kill(self.monitor)
        self.monitor = None

    @staticmethod
    def _kill(proc):
        if os.name == "nt":  # 連同它開出來的子程序一起關
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)], capture_output=True, creationflags=NO_WINDOW)
        else:
            proc.terminate()

    # ---- 在區域網路上找板子 ---------------------------------------------
    def _listen_boards(self):
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind(("", UDP_PORT))
        except OSError as e:
            self.after(0, self.found_label.config, {"text": f"沒辦法監聽 UDP {UDP_PORT}:{e}"})
            return
        while True:
            try:
                data, _ = s.recvfrom(256)
            except OSError:
                return
            msg = data.decode("utf-8", "replace")
            if not msg.startswith("ESP32_IP:"):
                continue
            parts = msg[len("ESP32_IP:"):].split(";")
            info = dict(p.split("=", 1) for p in parts[1:] if "=" in p)
            self.found[parts[0]] = info
            self.after(0, self._show_found, parts[0], info)

    def _show_found(self, ip, info):
        ble = "藍牙已連上主機" if info.get("ble") == "1" else "藍牙等待配對"
        self.last_ip = ip
        self.found_label.config(fg=TEXT, text=f"找到板子  {info.get('name', '')}   IP {ip}   {ble}   韌體 {info.get('fw', '?')}")

    def open_web(self):
        ip = getattr(self, "last_ip", None)
        if ip:
            webbrowser.open(f"http://{ip}/")
        else:
            self.say("還沒找到板子。電腦和板子要在同一個 WiFi。\n", "err")

    def _on_close(self):
        self.stop()
        self.close_monitor()
        self.destroy()


if __name__ == "__main__":
    if not INI.exists():
        sys.exit(f"找不到 {INI}")
    Panel().mainloop()
