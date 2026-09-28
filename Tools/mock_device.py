#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
模拟 ADS1115 设备：作为 TCP 客户端连接上位机，发送 hello 与模拟采样，
应答 set_ch / reset_cfg / get_cfg / set_net 命令。

用途：无硬件联调、上位机截图、协议回归测试。

用法：
    python mock_device.py [--host 127.0.0.1] [--port 9000] [--seconds 0]

模拟通道（与产品默认一致）：
    CH1 电池电压：volt ≈ 1.33 V（≈ 4.0V 电池 ÷3），8 SPS
    CH2 缓慢正弦：volt ≈ 1.65 V ± 0.05，16 SPS
"""

import argparse
import json
import math
import random
import socket
import sys
import threading
import time

HELLO_CFG = [
    {"ch": 0, "mux": 0, "pga": 4, "dr": 4, "enabled": False},
    {"ch": 1, "mux": 5, "pga": 2, "dr": 0, "enabled": True},
    {"ch": 2, "mux": 6, "pga": 1, "dr": 1, "enabled": True},
    {"ch": 3, "mux": 5, "pga": 2, "dr": 2, "enabled": False},
]
PGA_FSR = (6.144, 4.096, 2.048, 1.024, 0.512, 0.256)


class MockDevice:
    def __init__(self, host, port):
        self.host = host
        self.port = port
        self.cfg = {c["ch"]: dict(c) for c in HELLO_CFG}
        self.seq = 0
        self.sock: socket.socket | None = None
        self.send_lock = threading.Lock()
        self.stop_evt = threading.Event()

    # ---------------- 帧发送 ----------------

    def send(self, obj):
        line = json.dumps(obj, ensure_ascii=False) + "\n"
        try:
            with self.send_lock:
                if self.sock is not None:
                    self.sock.sendall(line.encode("utf-8"))
        except OSError:
            pass

    def send_hello(self):
        self.send({"type": "hello", "fw": "2.0",
                   "cfg": [self.cfg[i] for i in sorted(self.cfg)]})

    # ---------------- 采样 ----------------

    def sample_loop(self):
        fsr = {ch: PGA_FSR[c["pga"]] for ch, c in self.cfg.items()}
        start = time.monotonic()
        last_sent = {}

        while not self.stop_evt.is_set():
            now = time.monotonic()
            dev_ms = int((now - start) * 1000)

            for ch, c in self.cfg.items():
                if not c["enabled"]:
                    continue
                dr = (8, 16, 32, 64, 128, 250, 475, 860)[c["dr"]]
                period = 1.0 / dr
                if now - last_sent.get(ch, 0.0) < period:
                    continue
                last_sent[ch] = now

                if ch == 1:
                    volt = 1.3270 + random.uniform(-0.0004, 0.0004)
                elif ch == 2:
                    volt = 1.6500 + 0.050 * math.sin(now / 6.0) \
                        + random.uniform(-0.001, 0.001)
                else:
                    volt = 0.500 + random.uniform(-0.002, 0.002)

                raw = int(max(-32768, min(32767,
                          volt / PGA_FSR[c["pga"]] * 32768)))
                self.seq += 1
                self.send({"dev_ms": dev_ms, "ch": ch, "raw": raw,
                           "volt": round(volt, 6), "seq": self.seq})
            time.sleep(0.02)

    # ---------------- 命令 ----------------

    def handle_line(self, line: str):
        try:
            obj = json.loads(line)
        except ValueError:
            return
        cmd = obj.get("cmd")
        if cmd == "set_ch":
            ch = int(obj.get("ch", -1))
            if ch in self.cfg:
                for key in ("mux", "pga", "dr"):
                    self.cfg[ch][key] = int(obj.get(key, self.cfg[ch][key]))
                self.cfg[ch]["enabled"] = bool(obj.get("enabled"))
                print(f"[mock] set_ch CH{ch} -> {self.cfg[ch]}", flush=True)
            self.send({"type": "ack", "cmd": "set_ch", "ok": True})
        elif cmd == "reset_cfg":
            for c in HELLO_CFG:
                self.cfg[c["ch"]] = dict(c)
            self.send({"type": "ack", "cmd": "reset_cfg", "ok": True})
            self.send_hello()
        elif cmd == "get_cfg":
            self.send_hello()
        elif cmd == "set_net":
            print(f"[mock] set_net ssid={obj.get('ssid')} (模拟，不切换)",
                  flush=True)
            self.send({"type": "ack", "cmd": "set_net", "ok": True,
                       "detail": "applying (mock)"})
        else:
            print(f"[mock] 收到未知命令: {line.strip()}", flush=True)

    def recv_loop(self):
        buf = b""
        while not self.stop_evt.is_set():
            try:
                self.sock.settimeout(0.5)
                data = self.sock.recv(4096)
            except socket.timeout:
                continue
            except (OSError, AttributeError):
                return
            if not data:
                print("[mock] recv EOF", flush=True)
                return
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                self.handle_line(line.decode("utf-8", "replace"))

    # ---------------- 连接 ----------------

    def run(self, seconds: float):
        worker = None
        deadline = (time.monotonic() + seconds) if seconds > 0 else None

        while not self.stop_evt.is_set():
            if deadline is not None and time.monotonic() > deadline:
                break
            try:
                print(f"[mock] 连接 {self.host}:{self.port} …", flush=True)
                self.sock = socket.create_connection(
                    (self.host, self.port), timeout=5)
            except OSError as e:
                print(f"[mock] 连接失败：{e}，3s 后重试", flush=True)
                time.sleep(3)
                continue

            print("[mock] 已连接", flush=True)
            self.send_hello()
            worker = threading.Thread(target=self.sample_loop, daemon=True)
            worker.start()
            threading.Thread(target=self.recv_loop, daemon=True).start()

            # 探测连接存活（MSG_PEEK 不吞数据，实际解析在 recv_loop）
            while not self.stop_evt.is_set():
                if deadline is not None and time.monotonic() > deadline:
                    self.stop_evt.set()
                try:
                    self.sock.settimeout(0.5)
                    if self.sock.recv(1, socket.MSG_PEEK) == b"":
                        print("[mock] peek: 对端已关闭", flush=True)
                        break
                except socket.timeout:
                    continue
                except OSError as e:
                    print(f"[mock] peek OSError: {e}", flush=True)
                    break
                time.sleep(0.2)

            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None
            print("[mock] 断开，重连中…", flush=True)
            time.sleep(2)

        self.stop_evt.set()


def main():
    parser = argparse.ArgumentParser(description="ADS1115 模拟设备")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--seconds", type=float, default=0,
                        help="运行秒数，0 表示一直运行")
    args = parser.parse_args()

    dev = MockDevice(args.host, args.port)
    try:
        dev.run(args.seconds)
    except KeyboardInterrupt:
        dev.stop_evt.set()
    return 0


if __name__ == "__main__":
    sys.exit(main())
