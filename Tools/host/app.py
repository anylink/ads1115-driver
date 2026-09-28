#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
主窗口组装（composition root）：
  - 拥有全部业务状态（采样缓存、设备配置、本地配置、CSV 记录器）
  - 网络线程只搬运帧，所有业务语义在 GUI 线程处理
  - 视图（AnalyzerView）只负责画，数据由这里推
"""

import queue
import sys
import time
from collections import deque
from pathlib import Path

import numpy as np
import pyqtgraph as pg
from PySide6.QtCore import Qt, QEventLoop, QTimer
from PySide6.QtGui import QAction
from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
    QComboBox,
    QDialog,
    QLabel,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QStatusBar,
    QVBoxLayout,
    QWidget,
    QHBoxLayout,
)

from . import protocol
from .analyzer_view import AnalyzerView
from .config_dialog import DeviceConfigDialog, NetworkDialog
from .csvlog import CsvRecorder
from .net import TcpServer, MdnsAdvertiser, list_interfaces, pick_primary_interface
from .storage import AppStore, app_base_dir

import logging
import traceback

try:   # 文件调试日志：与 exe 同级的 debug.log（窗口程序 stderr 不可见，留此为证）
    logging.basicConfig(
        filename=str(app_base_dir() / "debug.log"),
        level=logging.DEBUG,
        format="%(asctime)s.%(msecs)03d %(message)s",
        datefmt="%H:%M:%S")
except Exception:
    pass
log = logging.getLogger("host")

APP_VERSION = "2.3"
SERIES_MAXLEN = 600000
RATE_WINDOW_S = 5.0


class MainWindow(QMainWindow):
    def __init__(self, port):
        super().__init__()
        self.setWindowTitle(f"ADS1115 上位机 v{APP_VERSION}")
        self.resize(1300, 860)
        self.setMinimumSize(1100, 840)   # 保证四行信息框 + 波形有足够行高

        self.port = port
        self.store = AppStore()
        self.recorder = CsvRecorder()

        self.series = {}        # ch -> {"t","volt","raw","seq"} deque
        self.latest = {}        # ch -> Sample
        self.device_cfg = {}    # ch -> {"mux","pga","dr","enabled"}
        self.connected = False
        self.fw = ""
        self.peer = ""
        self.recv_mono = deque(maxlen=4000)
        self.pkt_ok = 0
        self.pkt_bad = 0
        self._curve_len = {}    # ch -> 上次重绘时的点数
        self._ack_needed = 0
        self._ack_got = 0
        self._ack_loop: QEventLoop | None = None

        self.frames_q = queue.Queue()
        self.events_q = queue.Queue()
        self.send_q = queue.Queue()
        self.server: TcpServer | None = None
        self.mdns: MdnsAdvertiser | None = None
        self.interfaces: list = []
        self._mdns_msg = ""

        self._build_ui()
        self.ui_timer = QTimer(self)
        self.ui_timer.timeout.connect(self._refresh)
        self.ui_timer.start(50)

    # ---------------- UI ----------------

    def _build_ui(self):
        self._build_menu()

        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)

        top = QHBoxLayout()
        self.conn_label = QLabel("等待设备连接…")
        self.conn_label.setStyleSheet("color:#b58100; font-weight:bold;")
        self.rate_label = QLabel("0.0 样本/s")
        self.pkt_label = QLabel("收包 0")
        top.addWidget(self.conn_label)
        top.addWidget(self.rate_label)
        top.addWidget(self.pkt_label)
        top.addStretch()

        top.addWidget(QLabel("通告 IP"))
        self.ip_combo = QComboBox()
        self.ip_combo.setToolTip("mDNS 向设备通告的本机地址；虚拟网卡已标注")
        self.ip_combo.currentIndexChanged.connect(self._on_ip_changed)
        top.addWidget(self.ip_combo)

        top.addWidget(QLabel("窗口"))
        self.win_combo = QComboBox()
        for sec in (30, 60, 120, 300, 600):
            self.win_combo.addItem(f"{sec} s", sec)
        self.win_combo.setCurrentIndex(2)
        top.addWidget(self.win_combo)

        self.freeze_chk = QCheckBox("冻结")
        self.follow_chk = QCheckBox("跟随最新")
        self.follow_chk.setChecked(True)
        top.addWidget(self.freeze_chk)
        top.addWidget(self.follow_chk)

        clear_btn = QPushButton("清除曲线")
        clear_btn.clicked.connect(self._clear_curves)
        top.addWidget(clear_btn)

        self.rec_btn = QPushButton("● 开始记录 CSV")
        self.rec_btn.setStyleSheet(
            "QPushButton{color:#fff;background:#1a7f37;padding:4px 14px;"
            "border-radius:4px;font-weight:bold;}"
            "QPushButton:hover{background:#146629;}")
        self.rec_btn.clicked.connect(self._toggle_record)
        top.addWidget(self.rec_btn)
        root.addLayout(top)

        self.view = AnalyzerView()
        self.view.config_cb = self._on_inline_config
        root.addWidget(self.view, 1)

        self.setStatusBar(QStatusBar())
        self.statusBar().showMessage(f"等待设备连接…   |   TCP {self.port} 端口")

    def _build_menu(self):
        bar = self.menuBar()

        m_cfg = bar.addMenu("配置(&C)")
        act_dev = QAction("设备配置…", self)
        act_dev.triggered.connect(self._on_open_config)
        m_cfg.addAction(act_dev)
        act_net = QAction("网络配置…", self)
        act_net.triggered.connect(self._on_open_network)
        m_cfg.addAction(act_net)

        m_view = bar.addMenu("视图(&V)")
        act_clear = QAction("清除曲线", self)
        act_clear.triggered.connect(self._clear_curves)
        m_view.addAction(act_clear)

        m_help = bar.addMenu("帮助(&H)")
        act_about = QAction("关于…", self)
        act_about.triggered.connect(self._on_about)
        m_help.addAction(act_about)

    def _on_about(self):
        QMessageBox.about(
            self, "关于 ADS1115 上位机",
            f"<b>ADS1115 上位机 v{APP_VERSION}</b><br><br>"
            "配合 ESP32 设备（固件协议 v2）使用的四通道 ADC 实时监测工具。<br><br>"
            "<b>功能</b>：逻辑分析仪式多通道波形与实时数值、设备通道配置下发"
            "（MUX/PGA/数据率/使能，断电保存）、WiFi 远程配置（失败自动回滚）、"
            "CSV 数据记录（带时间戳）。<br><br>"
            "<b>协议</b>：TCP JSON 行协议 v2，mDNS 自动发现（ads1115-host）。<br>"
            "<b>数据</b>：设备只输出原始码值与引脚电压，工程值换算在本机"
            " config/host_config.json 中按通道配置。<br><br>"
            "示例工程：ads1115-driver / Tools")

    def start_background(self):
        self.server = TcpServer(self.port, self.frames_q,
                                self.events_q, self.send_q)
        self.server.start()

        # 网卡枚举（PowerShell 较慢）放后台线程，窗口先显示先响应
        def load_interfaces():
            self.events_q.put(("interfaces", list_interfaces()))

        import threading
        threading.Thread(target=load_interfaces, daemon=True,
                         name="if-list").start()

    def shutdown(self):
        self.recorder.stop()
        if self.server is not None:
            self.server.stop()
        if self.mdns is not None:
            self.mdns.close()

    def closeEvent(self, event):
        self.shutdown()
        super().closeEvent(event)

    # ---------------- 事件与帧处理 ----------------

    def _refresh(self):
        now_wall = time.time()
        now_mono = time.monotonic()

        # 连接/网卡事件
        while True:
            try:
                kind, payload = self.events_q.get_nowait()
            except queue.Empty:
                break
            if kind == "connect":
                self.connected = True
                self.peer = str(payload)
                self.fw = ""
                self.conn_label.setText(f"已连接：{self.peer}")
                self.conn_label.setStyleSheet(
                    "color:#1a7f37; font-weight:bold;")
            elif kind == "disconnect":
                self.connected = False
                self.conn_label.setText("已断开，等待设备重连…")
                self.conn_label.setStyleSheet(
                    "color:#c62828; font-weight:bold;")
            elif kind == "listening":
                self.statusBar().showMessage(
                    f"{self._mdns_msg}   |   TCP {self.port} 端口监听中")
            elif kind == "interfaces":
                self._apply_interfaces(payload)
            elif kind == "error":
                self.statusBar().showMessage(str(payload))

        # 数据帧
        while True:
            try:
                _, mono, line = self.frames_q.get_nowait()
            except queue.Empty:
                break
            frame = protocol.parse_frame(line)
            if frame is None:
                self.pkt_bad += 1
                continue
            self._handle_frame(frame, now_wall, mono)

        # 采样率
        while self.recv_mono and (now_mono - self.recv_mono[0]) > RATE_WINDOW_S:
            self.recv_mono.popleft()
        rate = len(self.recv_mono) / RATE_WINDOW_S
        self.rate_label.setText(f"{rate:.1f} 样本/s")
        rec_info = (f"   |   CSV 已记录 {self.recorder.rows} 行"
                    if self.recorder.recording else "")
        self.pkt_label.setText(
            f"收包 {self.pkt_ok}（坏帧 {self.pkt_bad}）{rec_info}")

        # 视图刷新
        if not self.freeze_chk.isChecked():
            for ch, row in self.view.rows.items():
                ser = self.series.get(ch)
                if ser is None or not ser["t"]:
                    continue
                n = len(ser["t"])
                if n != self._curve_len.get(ch):
                    t_arr = np.asarray(ser["t"], dtype=float)
                    local = self.store.conversion(ch)
                    t_arr = np.asarray(ser["t"], dtype=float)
                    v_arr = (np.asarray(ser["volt"], dtype=float)
                             * local.scale + local.offset)   # 曲线画换算后的真实值
                    self.view.update_curve(ch, t_arr, v_arr)
                    self._curve_len[ch] = n
                self._update_label(ch)
            if (self.follow_chk.isChecked() and self.view.has_rows()
                    and self.connected):
                window = self.win_combo.currentData()
                self.view.set_x_range_all(now_wall - window, now_wall)

    def _handle_frame(self, frame, now_wall, mono):
        ftype = frame.get("type")

        if ftype == "hello":
            self.fw = str(frame.get("fw", "?"))
            self.device_cfg = {}
            for item in frame.get("cfg", []):
                try:
                    self.device_cfg[int(item["ch"])] = {
                        "mux": int(item["mux"]), "pga": int(item["pga"]),
                        "dr": int(item["dr"]),
                        "enabled": bool(item.get("enabled")),
                    }
                except (KeyError, ValueError, TypeError):
                    continue
            self._rebuild_view()
            # hello 回执：设备收到任意命令即停止补发（幂等，重复收到也无副作用）
            log.info("hello: fw=%s cfg=%s", self.fw,
                     {c: v["enabled"] for c, v in self.device_cfg.items()})
            self.send_q.put(protocol.build_ping())
            if getattr(self, "_hello_loop", None) is not None:
                self._hello_loop.quit()
            if self.peer:
                self.conn_label.setText(
                    f"已连接：{self.peer}" + (f"   固件 v{self.fw}" if self.fw else ""))
            return

        if ftype == "ack":
            ok = bool(frame.get("ok"))
            cmd = str(frame.get("cmd", "?"))
            log.info("ack: cmd=%s ok=%s pending=%s (%d/%d)",
                     cmd, ok, self._ack_loop is not None,
                     self._ack_got, self._ack_needed)
            if self._ack_loop is not None and cmd == "set_ch":
                self._ack_got += 1
                if self._ack_got >= self._ack_needed:
                    self._ack_loop.quit()
            detail = frame.get("detail", "")
            self.statusBar().showMessage(
                f"设备应答 {cmd}: {'成功' if ok else '失败'} {detail}", 5000)
            return

        if ftype == "event":
            name = str(frame.get("name", "?"))
            text = {"net_applied": "设备已切换到新 WiFi",
                    "net_rolled_back": "设备换网失败，已自动回滚旧配置"}.get(
                        name, name)
            self.statusBar().showMessage(f"设备事件：{text}", 10000)
            return

        sample = protocol.make_sample(frame)
        if sample is None:
            self.pkt_bad += 1
            return
        self.pkt_ok += 1
        self.recv_mono.append(mono)

        ser = self.series.setdefault(sample.ch, {
            k: deque(maxlen=SERIES_MAXLEN) for k in ("t", "volt", "raw", "seq")
        })
        ser["t"].append(now_wall)
        ser["volt"].append(sample.volt)
        ser["raw"].append(sample.raw)
        ser["seq"].append(sample.seq)
        self.latest[sample.ch] = sample

        # CSV：通道使能 ∧ 用户记录开关（换算在主机侧）
        if self.recorder.recording and self._record_allowed(sample.ch):
            local = self.store.conversion(sample.ch)
            value = sample.volt * local.scale + local.offset
            self.recorder.write_row(
                sample.dev_ms, sample.ch, local.name,
                sample.raw, sample.volt, value, sample.seq)

    def _record_allowed(self, ch: int) -> bool:
        cfg = self.device_cfg.get(ch)
        return bool(cfg and cfg["enabled"]) and self.store.conversion(ch).save

    # ---------------- 视图联动 ----------------

    def _rebuild_view(self):
        specs = []
        for ch in range(4):
            cfg = self.device_cfg.get(
                ch, {"mux": 4, "pga": 2, "dr": 4, "enabled": False})
            local = self.store.conversion(ch)
            fsr = protocol.PGA_FSR[cfg["pga"]]
            y_lo, y_hi = (-fsr, fsr) if cfg["mux"] < 4 else (0.0, fsr)
            # 曲线/纵轴显示换算后的真实值，量程也随之换算
            cy_lo = y_lo * local.scale + local.offset
            cy_hi = y_hi * local.scale + local.offset
            specs.append((ch, local.name, local.unit,
                          cy_lo, cy_hi, cfg["enabled"]))
        self.view.rebuild(specs)
        self._curve_len = {}
        for ch in self.view.rows:
            self.view.set_channel_controls(ch, self.store.conversion(ch))
            self._update_label(ch)

    def _update_label(self, ch: int):
        cfg = self.device_cfg.get(
            ch, {"mux": 4, "pga": 2, "dr": 4, "enabled": False})
        if ch not in self.view.rows:
            return
        local = self.store.conversion(ch)
        fsr = protocol.PGA_FSR[cfg["pga"]]
        y_lo, y_hi = (-fsr, fsr) if cfg["mux"] < 4 else (0.0, fsr)
        cy_lo = y_lo * local.scale + local.offset
        cy_hi = y_hi * local.scale + local.offset

        sample = self.latest.get(ch)
        if sample is None or not cfg["enabled"]:
            value_text = "--"
            raw_text = "--"
            volt_text = "--"
        else:
            value = sample.volt * local.scale + local.offset
            value_text = f"{value:.{local.decimals}f}"
            raw_text = str(sample.raw)
            av = abs(sample.volt)
            volt_text = (f"{sample.volt * 1000:.1f} mV" if av < 1.0
                         else f"{sample.volt:.3f} V")   # 自适应单位，与纵轴一致
        self.view.set_label(ch, local.name, value_text, local.unit,
                            cy_lo, cy_hi, enabled=cfg["enabled"],
                            raw_text=raw_text, volt_text=volt_text)

    def _apply_interfaces(self, candidates):
        if not candidates:
            return
        self.interfaces = candidates
        primary_ip, primary_alias = pick_primary_interface(candidates)

        from .net import is_virtual_adapter
        combo = self.ip_combo
        combo.blockSignals(True)
        combo.clear()
        chosen = 0
        for i, (alias, ip) in enumerate(candidates):
            tag = "（虚拟网卡）" if is_virtual_adapter(alias) else ""
            combo.addItem(f"{ip}  [{alias}]{tag}", ip)
            if ip == primary_ip and chosen == 0:
                chosen = i
        combo.setCurrentIndex(chosen)
        combo.blockSignals(False)

        self.mdns = MdnsAdvertiser(self.port)
        import os
        if os.environ.get("ADS1115_NO_MDNS") == "1":
            self._mdns_msg = "mDNS 已禁用（测试模式）"
        elif self.mdns.start(primary_ip):
            self._mdns_msg = f"mDNS 已注册 → {primary_ip} [{primary_alias}]"
        else:
            self._mdns_msg = "mDNS 注册失败（防火墙拦了 UDP 5353？），设备端可改用固定 IP"
        self.statusBar().showMessage(
            f"{self._mdns_msg}   |   TCP {self.port} 端口监听中")

    def _on_ip_changed(self, idx):
        if idx < 0 or self.mdns is None:
            return
        ip = self.ip_combo.itemData(idx)
        if ip and self.mdns.update(ip):
            self.statusBar().showMessage(
                f"通告 IP 已切换为 {ip}，设备将在下次重连时使用（约 3 s）", 5000)

    # ---------------- 交互 ----------------

    def _on_inline_config(self, ch: int, values: dict):
        """行内配置控件变更：立即生效（比例/偏移/单位/记录开关），并存盘"""
        local = self.store.conversion(ch)
        local.save = values["save"]
        local.unit = values["unit"]
        local.scale = values["scale"]
        local.offset = values["offset"]
        self.store.channels[ch] = local
        self.store.save()
        # 换算变了：纵轴量程与曲线都要按新换算重画
        cfg = self.device_cfg.get(
            ch, {"mux": 4, "pga": 2, "dr": 4, "enabled": False})
        fsr = protocol.PGA_FSR[cfg["pga"]]
        y_lo, y_hi = (-fsr, fsr) if cfg["mux"] < 4 else (0.0, fsr)
        cy_lo = y_lo * local.scale + local.offset
        cy_hi = y_hi * local.scale + local.offset
        self.view.set_y_range(ch, cy_lo, cy_hi)
        self._curve_len[ch] = -1   # 强制曲线按新换算重画
        self._update_label(ch)

    def _on_open_config(self):
        log.info("open config dialog: connected=%s device_cfg=%s",
                 self.connected, self.device_cfg)
        if not self.connected:
            QMessageBox.information(self, "设备未就绪",
                                    "设备尚未连接，请确认设备已上电并接入网络。")
            return

        if not self.device_cfg:
            # hello 兜底：主动向设备要一次配置，短暂等待
            self.send_q.put(protocol.build_get_cfg())
            loop = QEventLoop()
            self._hello_loop = loop
            QTimer.singleShot(2000, loop.quit)
            loop.exec()
            self._hello_loop = None
            if not self.device_cfg:
                QMessageBox.information(
                    self, "设备未就绪",
                    "仍未收到设备配置（hello）。请确认设备固件为 v2 并已重新"
                    "烧录；若持续出现，请重启设备与上位机。")
                return

        try:
            dlg = DeviceConfigDialog(self, self.device_cfg, self.store)
        except Exception as e:   # 防御：对话框构建失败不应无声无息
            QMessageBox.warning(self, "打开失败", f"配置面板构建异常：{e}")
            return
        dlg.request_reset = False
        dlg.net_result = None
        dlg.move(self.screen().availableGeometry().center() - dlg.rect().center())
        log.info("dialog exec start")
        accepted = dlg.exec()
        log.info("dialog exec done accepted=%s reset=%s net=%s",
                 accepted, dlg.request_reset, dlg.net_result)
        if accepted != QDialog.DialogCode.Accepted:
            return

        if dlg.request_reset:
            self.send_q.put(protocol.build_reset_cfg())
            self.send_q.put(protocol.build_get_cfg())
            self.statusBar().showMessage("已发送恢复默认命令，配置以后续 hello 为准", 6000)
            return

        if dlg.net_result is not None:
            ssid, passwd = dlg.net_result
            self.send_q.put(protocol.build_set_net(ssid, passwd))
            self.statusBar().showMessage(
                "已发送换网请求，等待设备应用/回滚事件…", 8000)

        cfgs = dlg.device_result()
        log.info("dialog accepted, cfgs=%s", cfgs)
        try:
            ok, detail = self._send_channel_cfgs(cfgs)
        except Exception:
            log.exception("send_channel_cfgs exception")
            QMessageBox.critical(
                self, "下发异常",
                f"下发过程出现异常，详情见 debug.log。")
            return
        # 无论应答是否齐全，都请设备回传当前配置（hello）做最终确认
        log.info("send result ok=%s detail=%s", ok, detail)
        self.send_q.put(protocol.build_get_cfg())
        if not ok:
            QMessageBox.warning(
                self, "下发失败",
                f"部分配置未收到设备应答（{detail}）。已请求设备回传当前配置，"
                f"主页面将按设备实际状态刷新；若仍未生效请检查设备。")
        for c in cfgs:
            self.device_cfg[c["ch"]] = {
                "mux": c["mux"], "pga": c["pga"],
                "dr": c["dr"], "enabled": c["enabled"],
            }
            local = self.store.conversion(c["ch"])
            local.name = c["name"]
            self.store.channels[c["ch"]] = local
        self.store.save()
        self._rebuild_view()
        self.statusBar().showMessage("通道配置已下发并保存", 5000)

    def _on_open_network(self):
        """独立入口：仅修改设备 WiFi（带回滚）"""
        if not self.connected:
            QMessageBox.information(self, "设备未就绪",
                                    "设备尚未连接，无法下发网络配置。")
            return
        dlg = NetworkDialog(self)
        dlg.move(self.screen().availableGeometry().center() - dlg.rect().center())
        if dlg.exec() != QDialog.DialogCode.Accepted or not dlg.result_ssid:
            return
        self.send_q.put(protocol.build_set_net(dlg.result_ssid, dlg.result_pass))
        self.statusBar().showMessage("已发送换网请求，等待设备应用/回滚事件…", 8000)

    def _send_channel_cfgs(self, cfgs):
        self._ack_needed = len(cfgs)
        self._ack_got = 0
        log.info("sending %d set_ch commands", len(cfgs))
        for c in cfgs:
            line = protocol.build_set_ch(
                c["ch"], c["mux"], c["pga"], c["dr"], c["enabled"])
            self.send_q.put(line)
            log.info("sent: %s", line)
        self.statusBar().showMessage(
            f"正在下发 {len(cfgs)} 条通道配置，等待设备应答…", 8000)
        loop = QEventLoop()
        self._ack_loop = loop
        QTimer.singleShot(5000, loop.quit)
        loop.exec()
        self._ack_loop = None
        if self._ack_got >= self._ack_needed:
            return True, f"{self._ack_got}/{self._ack_needed}"
        return False, f"仅收到 {self._ack_got}/{self._ack_needed} 个应答"

    def _toggle_record(self):
        if self.recorder.recording:
            path = self.recorder.path
            self.recorder.stop()
            self.rec_btn.setText("● 开始记录 CSV")
            self.rec_btn.setStyleSheet(
                "QPushButton{color:#fff;background:#1a7f37;padding:4px 14px;"
                "border-radius:4px;font-weight:bold;}"
                "QPushButton:hover{background:#146629;}")
            self.statusBar().showMessage(
                f"已停止记录，共 {self.recorder.rows} 行 → {path}", 8000)
        else:
            path = self.recorder.start(app_base_dir() / "csv_out")
            self.rec_btn.setText("■ 停止记录 CSV")
            self.rec_btn.setStyleSheet(
                "QPushButton{color:#fff;background:#c62828;padding:4px 14px;"
                "border-radius:4px;font-weight:bold;}"
                "QPushButton:hover{background:#a02020;}")
            self.statusBar().showMessage(f"记录中 → {path}", 8000)

    def _clear_curves(self):
        for ser in self.series.values():
            for dq in ser.values():
                dq.clear()
        self._curve_len = {}
        self.view.clear_data()


def _icon_path():
    """定位 app.ico：onefile 解包目录 → exe 旁 → 源码树"""
    meipass = getattr(sys, "_MEIPASS", None)
    if meipass:
        p = Path(meipass) / "app.ico"
        if p.exists():
            return p
    if getattr(sys, "frozen", False):
        p = Path(sys.executable).resolve().parent / "app.ico"
        if p.exists():
            return p
    return Path(__file__).resolve().parent.parent / "app.ico"


def main():
    import argparse

    parser = argparse.ArgumentParser(description="ADS1115 上位机")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--smoke", action="store_true", help="无头自检后退出")
    args = parser.parse_args()

    pg.setConfigOptions(background="w", foreground="k", antialias=True)
    app = QApplication(sys.argv)
    app.setApplicationName("ads1115-host")

    icon_file = _icon_path()
    if icon_file.exists():
        from PySide6.QtGui import QIcon
        app.setWindowIcon(QIcon(str(icon_file)))

    win = MainWindow(port=args.port)

    if args.smoke:
        win.start_background()
        for _ in range(6):
            app.processEvents()
            time.sleep(0.05)
        win._refresh()
        assert win.server.is_alive(), "TCP 服务线程未运行"
        win.shutdown()
        print("SMOKE OK")
        return 0

    win.show()               # 窗口先弹出来
    win.start_background()   # TCP 立即就绪；网卡/mDNS 后台补齐
    ret = app.exec()
    win.shutdown()
    return ret
