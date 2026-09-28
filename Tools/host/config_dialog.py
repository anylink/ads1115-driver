#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
设备配置对话框：四路的设备侧参数（使能/通道名/MUX/PGA/数据率）+ 网络配置入口。

主机侧换算配置（记录开关/单位/小数位/scale/offset）已内联到主页面
每个通道的信息框里，不在此对话框中。

MUX 冲突校验在这里做（驱动故意不做物理层检查）：
同一物理引脚被两个使能通道占用即视为冲突，阻止下发。
"""

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QFormLayout,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMessageBox,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from . import protocol


def mux_conflict_error(rows) -> str | None:
    """rows: [(ch, mux, pga, dr, enabled)]；返回冲突描述或 None"""
    usage: dict[int, list[int]] = {}
    for row in rows:
        ch, mux, enabled = row[0], row[1], row[-1]   # 兼容 3 元/5 元两种行格式
        if not enabled:
            continue
        for pin in protocol.mux_pins(mux):
            usage.setdefault(pin, []).append(ch)
    for pin, chs in usage.items():
        if len(chs) > 1:
            return (f"物理引脚 AIN{pin} 被通道 "
                    f"{'、'.join(f'CH{c}' for c in chs)} 同时占用（MUX 冲突）")
    return None


def cycle_estimate_s(rows) -> float:
    """整轮采样周期 ≈ 各使能通道转换时间之和（含 2ms 余量）"""
    total = 0.0
    for _ch, _mux, pga, dr, enabled in rows:
        if enabled:
            total += (1000.0 / protocol.DR_SPS[dr]) + 2.0
    return total / 1000.0


class _ChannelRow:
    """对话框里一个通道的设备侧控件（通道名 + 使能 + MUX/PGA/DR）"""

    def __init__(self, ch: int, dev: dict, name: str):
        self.ch = ch
        self.enabled = QCheckBox()
        self.enabled.setChecked(bool(dev["enabled"]))
        self.name = QLineEdit(name)
        self.mux = QComboBox()
        for text, val in protocol.MUX_OPTIONS:
            self.mux.addItem(text, val)
        self.mux.setCurrentIndex(max(0, self.mux.findData(int(dev["mux"]))))
        self.pga = QComboBox()
        for text, val in protocol.PGA_OPTIONS:
            self.pga.addItem(text, val)
        self.pga.setCurrentIndex(max(0, self.pga.findData(int(dev["pga"]))))
        self.dr = QComboBox()
        for text, val in protocol.DR_OPTIONS:
            self.dr.addItem(text, val)
        self.dr.setCurrentIndex(max(0, self.dr.findData(int(dev["dr"]))))

    def device_result(self) -> dict:
        return {
            "ch": self.ch,
            "name": self.name.text().strip() or f"CH{self.ch}",
            "mux": self.mux.currentData(),
            "pga": self.pga.currentData(),
            "dr": self.dr.currentData(),
            "enabled": self.enabled.isChecked(),
        }


class DeviceConfigDialog(QDialog):
    """四路的设备侧参数；主机侧换算在主页面各通道信息框内编辑"""

    def __init__(self, parent, device_cfg: dict, store):
        super().__init__(parent)
        self.setWindowTitle("设备配置")
        self.resize(640, 300)
        self.store = store
        self.request_reset = False
        self.net_result = None

        root = QVBoxLayout(self)
        grid_box = QGroupBox("通道（设备侧参数；换算/记录在主页面各行内编辑）")
        grid = QGridLayout(grid_box)
        grid.setHorizontalSpacing(10)

        headers = ["使能", "通道名", "测量方式(MUX)", "测量范围(PGA)", "数据率"]
        for col, text in enumerate(headers):
            grid.addWidget(QLabel(text), 0, col)

        self.rows: list[_ChannelRow] = []
        for ch in range(4):
            dev = device_cfg.get(ch, {"mux": 4, "pga": 2, "dr": 4,
                                      "enabled": False})
            row = _ChannelRow(ch, dev, store.conversion(ch).name)
            self.rows.append(row)
            r = ch + 1

            cell = QWidget()
            cell_layout = QHBoxLayout(cell)
            cell_layout.setContentsMargins(0, 0, 0, 0)
            ch_label = QLabel(f"CH{ch}")
            ch_label.setStyleSheet("font-weight:bold;")
            cell_layout.addWidget(ch_label)
            cell_layout.addWidget(row.enabled)
            grid.addWidget(cell, r, 0)
            grid.addWidget(row.name, r, 1)
            grid.addWidget(row.mux, r, 2)
            grid.addWidget(row.pga, r, 3)
            grid.addWidget(row.dr, r, 4)

        root.addWidget(grid_box)

        self.cycle_label = QLabel()
        self._update_cycle()
        for row in self.rows:
            row.enabled.toggled.connect(self._update_cycle)
            row.dr.currentIndexChanged.connect(self._update_cycle)
        root.addWidget(self.cycle_label)

        buttons = QHBoxLayout()
        reset_btn = QPushButton("恢复设备默认")
        reset_btn.clicked.connect(self._on_reset)
        net_btn = QPushButton("网络配置…")
        net_btn.clicked.connect(self._on_network)
        buttons.addWidget(reset_btn)
        buttons.addWidget(net_btn)
        buttons.addStretch()
        cancel = QPushButton("取消")
        cancel.clicked.connect(self.reject)
        apply_btn = QPushButton("下发并保存")
        apply_btn.setDefault(True)
        apply_btn.clicked.connect(self._on_apply)
        buttons.addWidget(cancel)
        buttons.addWidget(apply_btn)
        root.addLayout(buttons)

    # ---------------- 行为 ----------------

    def _all_rows(self):
        return [(r.ch, r.mux.currentData(), r.pga.currentData(),
                 r.dr.currentData(), r.enabled.isChecked())
                for r in self.rows]

    def _update_cycle(self):
        est = cycle_estimate_s(self._all_rows())
        self.cycle_label.setText(
            f"整轮采样周期 ≈ {est:.2f} s（四路分时复用，逐路转换时间之和）")

    def _on_apply(self):
        import logging
        logging.getLogger("host.dialog").info("apply clicked, rows=%s",
                                              self._all_rows())
        err = mux_conflict_error(self._all_rows())
        if err is not None:
            QMessageBox.warning(self, "MUX 冲突", f"{err}\n请先调整再下发。")
            return
        self.accept()

    def _on_reset(self):
        if QMessageBox.question(
                self, "恢复设备默认",
                "将把设备四路配置恢复为出厂默认并覆盖 NVS，确定？") == QMessageBox.Yes:
            self.request_reset = True
            self.accept()

    def _on_network(self):
        dlg = NetworkDialog(self)
        if dlg.exec() == QDialog.Accepted and dlg.result_ssid:
            self.net_result = (dlg.result_ssid, dlg.result_pass)
            QMessageBox.information(
                self, "已发送",
                "设备将尝试连接新 WiFi；约 60 秒内拿不到 IP 会自动回滚旧配置。\n"
                "结果会以事件形式显示在主窗口底部。")

    # ---------------- 结果 ----------------

    def device_result(self) -> list[dict]:
        return [r.device_result() for r in self.rows]


class NetworkDialog(QDialog):
    """WiFi SSID/密码修改（设备端带回滚；端口与主机名写死在固件）"""

    def __init__(self, parent):
        super().__init__(parent)
        self.setWindowTitle("网络配置")
        self.result_ssid = ""
        self.result_pass = ""

        form = QFormLayout(self)
        self.ssid_edit = QLineEdit()
        self.pass_edit = QLineEdit()
        self.pass_edit.setPlaceholderText("留空表示开放网络")
        form.addRow(QLabel("SSID"), self.ssid_edit)
        form.addRow(QLabel("密码"), self.pass_edit)
        hint = QLabel("仅可修改 SSID 与密码；端口/固定 IP/mDNS 名在固件编译期配置。\n"
                      "设备收到后立即切换，60 秒内失败自动回滚。")
        hint.setStyleSheet("color:#888;")
        form.addRow(hint)

        bb = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        bb.accepted.connect(self._on_ok)
        bb.rejected.connect(self.reject)
        form.addRow(bb)

    def _on_ok(self):
        ssid = self.ssid_edit.text().strip()
        if not ssid:
            QMessageBox.warning(self, "缺少 SSID", "SSID 不能为空。")
            return
        self.result_ssid = ssid
        self.result_pass = self.pass_edit.text()
        self.accept()
