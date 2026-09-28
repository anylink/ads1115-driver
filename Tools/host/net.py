#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
网络层：TCP 服务端（双向帧通道）+ mDNS 广告。

线程模型：
  - TcpServer 线程只搬运字节：收到的 JSON 行解析后连同接收时刻放进 frames_q，
    调用方要发的文本行放进 send_q，由收发循环统一发送。
  - 所有业务状态由 GUI 线程独占处理，本模块不碰任何业务语义。
"""

import queue
import socket
import threading
import time

try:
    from zeroconf import ServiceInfo, Zeroconf
    HAVE_ZEROCONF = True
except ImportError:
    HAVE_ZEROCONF = False

SERVICE_TYPE = "_ads1115._tcp.local."
SERVICE_NAME = "ads1115-host"


def local_ipv4s():
    """兜底：本机非回环 IPv4（拿不到网卡名信息时用）"""
    addrs = set()
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("10.255.255.255", 1))  # 不会真正发包
        addrs.add(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            addrs.add(info[4][0])
    except OSError:
        pass
    return sorted(a for a in addrs if a != "127.0.0.1")


VIRTUAL_IF_KEYWORDS = (
    "vmware", "virtualbox", "vethernet", "hyper-v", "wsl", "loopback",
    "tailscale", "zerotier", "hamachi", "virtual", "bluetooth", "蓝牙",
)


def is_virtual_adapter(alias: str) -> bool:
    a = (alias or "").lower()
    return any(k in a for k in VIRTUAL_IF_KEYWORDS)


def list_interfaces():
    """枚举本机 IPv4，返回 [(网卡名, IP)]；用于把 mDNS 绑到真实网卡"""
    candidates = []
    try:
        import subprocess
        out = subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             "Get-NetIPAddress -AddressFamily IPv4 | "
             "Where-Object { $_.IPAddress -notlike '127.*' "
             "-and $_.IPAddress -notlike '169.254.*' } | "
             "Select-Object IPAddress,InterfaceAlias | ConvertTo-Json -Compress"],
            capture_output=True, text=True, timeout=10,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        data = __import__("json").loads(out.stdout or "null")
        if isinstance(data, dict):
            data = [data]
        for item in data or []:
            ip = item.get("IPAddress")
            alias = item.get("InterfaceAlias") or "?"
            if ip:
                candidates.append((str(alias), str(ip)))
    except (OSError, ValueError):
        pass
    if not candidates:
        candidates = [("(default)", ip) for ip in local_ipv4s()]
    return candidates


def pick_primary_interface(candidates):
    for alias, ip in candidates:
        if not is_virtual_adapter(alias):
            return ip, alias
    return candidates[0]


class TcpServer(threading.Thread):
    """单客户端 TCP 服务端：断开后继续等待设备重连"""

    def __init__(self, port, frames_q, events_q, send_q):
        super().__init__(daemon=True, name="tcp-server")
        self.port = port
        self.frames_q = frames_q      # (recv_wall, recv_mono, frame|None)
        self.events_q = events_q      # (kind, payload)
        self.send_q = send_q          # str（含结尾换行由本模块补）
        self._stop = threading.Event()

    def stop(self):
        self._stop.set()

    def run(self):
        try:
            srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            srv.bind(("0.0.0.0", self.port))
            srv.listen(1)
            srv.settimeout(0.5)
        except OSError as e:
            self.events_q.put(("error", f"TCP {self.port} 端口监听失败: {e}"))
            return
        self.events_q.put(("listening", None))

        while not self._stop.is_set():
            try:
                conn, addr = srv.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            self._serve(conn, addr)
            self.events_q.put(("disconnect", addr[0]))
        srv.close()

    def _serve(self, conn, addr):
        self.events_q.put(("connect", addr[0]))
        buf = b""
        conn.settimeout(0.5)
        try:
            while not self._stop.is_set():
                try:
                    data = conn.recv(4096)
                except socket.timeout:
                    data = b""
                if data:
                    buf += data
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        self.frames_q.put((time.time(), time.monotonic(), line))
                if self._drain_send(conn) != 0:
                    break
        except OSError:
            pass
        finally:
            conn.close()

    def _drain_send(self, conn) -> int:
        while True:
            try:
                line = self.send_q.get_nowait()
            except queue.Empty:
                return 0
            payload = line if line.endswith("\n") else line + "\n"
            try:
                conn.sendall(payload.encode("utf-8"))
            except OSError:
                return -1


class MdnsAdvertiser:
    """注册 mDNS 服务，供设备自动发现本机（只通告选定的网卡地址）"""

    def __init__(self, port):
        self.port = port
        self.zc = None
        self.info = None
        self.ip = None

    def _register(self, ip):
        self.info = ServiceInfo(
            SERVICE_TYPE,
            f"{SERVICE_NAME}.{SERVICE_TYPE}",
            server=f"{SERVICE_NAME}.local.",
            port=self.port,
            properties={"proto": "json-line-v2"},
            addresses=[socket.inet_aton(ip)],
        )
        self.zc.register_service(self.info)
        self.ip = ip

    def start(self, ip) -> bool:
        if not HAVE_ZEROCONF:
            return False
        try:
            self.zc = Zeroconf()
            self._register(ip)
            return True
        except OSError:
            self.close()
            return False

    def update(self, ip) -> bool:
        if self.zc is None:
            return self.start(ip)
        if ip == self.ip:
            return True
        try:
            self.zc.unregister_service(self.info)
            self._register(ip)
            return True
        except OSError:
            return False

    def close(self):
        if self.zc is not None:
            try:
                if self.info is not None:
                    self.zc.unregister_service(self.info)
                self.zc.close()
            except OSError:
                pass
            self.zc = None
            self.info = None
