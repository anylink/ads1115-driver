#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
逻辑分析仪视图：四行常驻（每使能通道一行，未使能灰化），
每行拥有完整独立的坐标系（自己的底部时间轴 + 左侧电压轴），
时间游标（悬停/钉点）为贯穿全行的全局竖线。

左侧每行信息块版式（三段式）：
    名字 (CHx)                  ← 居中
    [大字数值框] [量程/RAW/VOLT]  ← 两个并排小框
    比例: [__] 偏移: [__] 记录☑   ← 内联配置一行
内联配置可在运行中直接修改——视图只收集变更并通过 config_cb 回调上报，
业务处理（存储、换算、CSV 过滤）全部在 App 侧。
"""

from datetime import datetime

import numpy as np
import pyqtgraph as pg
from PySide6.QtCore import Qt, QTimer
from PySide6.QtWidgets import (
    QAbstractSpinBox,
    QButtonGroup,
    QDoubleSpinBox,
    QFrame,
    QGraphicsProxyWidget,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QVBoxLayout,
)

CHANNEL_COLORS = {
    0: "#c7a500",   # 黄
    1: "#1a7f37",   # 绿
    2: "#1565c0",   # 蓝
    3: "#e07800",   # 橙
}
GRAY_TEXT = "#9aa0a6"
GRAY_CURVE = "#c9cdd2"
GRAY_BG = "#f4f5f7"
BOX_QSS = (
    "QFrame{border:1px solid #d9dce3; border-radius:5px; background:#fbfbfc;}"
    "QLabel{font-size:8pt; border:none; background:transparent;}"
    "QCheckBox{font-size:8pt; border:none; background:transparent;}"
    "QDoubleSpinBox{font-size:8pt; border:1px solid #d9dce3;"
    " border-radius:3px; background:white; padding:0px 1px;}"
    "QDoubleSpinBox:disabled{background:#f4f5f7; color:#c9cdd2;}"
)
VALUE_LABEL_QSS = (
    "QLabel{border:1px solid #d9dce3; border-radius:4px; background:white;"
    " color:%s; font-size:13pt; font-weight:bold;}"
)
INFO_LABEL_QSS = (
    "QLabel{border:1px solid #d9dce3; border-radius:4px; background:white;"
    " color:%s; font-size:8pt;}"
)


def channel_color(ch: int) -> str:
    return CHANNEL_COLORS.get(ch, "#555555")


class _Row:
    """一行的全部图元：三段式信息块（名/双框/配置行） + 独立坐标系波形区"""

    def __init__(self, ch: int, color: str):
        self.ch = ch
        self.color = color
        self.enabled = True

        # ---- 左侧三段式信息 + 内联配置块 ----
        self.box = QFrame()
        self.box.setStyleSheet(BOX_QSS)
        box_v = QVBoxLayout(self.box)
        box_v.setContentsMargins(8, 6, 8, 6)
        box_v.setSpacing(4)

        self.name_label = QLabel()
        self.name_label.setTextFormat(Qt.RichText)
        self.name_label.setAlignment(Qt.AlignCenter)
        box_v.addWidget(self.name_label)

        self.value_label = QLabel("--")
        self.value_label.setAlignment(Qt.AlignCenter)
        self.value_label.setFixedSize(100, 64)
        self.value_label.setStyleSheet(VALUE_LABEL_QSS % ("#555555",))

        self.info_label = QLabel()
        self.info_label.setTextFormat(Qt.RichText)
        self.info_label.setStyleSheet(INFO_LABEL_QSS % ("#666666",))
        self.info_label.setFixedSize(120, 64)

        mid = QHBoxLayout()
        mid.setSpacing(4)
        mid.addWidget(self.value_label)
        mid.addWidget(self.info_label)
        box_v.addLayout(mid)

        box_v.addStretch(1)

        self.scale_spin = QDoubleSpinBox()
        self.scale_spin.setRange(-1e6, 1e6)
        self.scale_spin.setDecimals(2)
        self.scale_spin.setFixedSize(64, 20)
        self.scale_spin.setButtonSymbols(QAbstractSpinBox.NoButtons)   # 纯输入框，无调节按钮
        self.scale_spin.setToolTip("比例 scale（真实值 = volt×scale+offset）")
        self.offset_spin = QDoubleSpinBox()
        self.offset_spin.setRange(-1e6, 1e6)
        self.offset_spin.setDecimals(2)
        self.offset_spin.setFixedSize(64, 20)
        self.offset_spin.setButtonSymbols(QAbstractSpinBox.NoButtons)
        self.offset_spin.setToolTip("偏移 offset")
        # 数据保存开关：用可按压按钮替代 QCheckBox——在带样式的容器里
        # QCheckBox 的勾选框渲染不可靠（曾整个消失），按钮状态由样式表
        # 明确控制（灰=关，绿=开），选中/未选中一眼可辨
        # 单位选择：V/A 分段按钮（QGraphicsProxyWidget 内嵌 QComboBox 的
        # 弹出列表会被场景裁剪遮挡，故用互斥按压按钮，无弹出层）
        self.unit_v_btn = QPushButton("V")
        self.unit_a_btn = QPushButton("A")
        for b in (self.unit_v_btn, self.unit_a_btn):
            b.setCheckable(True)
            b.setFixedSize(30, 20)
            b.setStyleSheet(
                "QPushButton{font-size:8pt; padding:0px;"
                " border:1px solid #c9cdd2; background:#f1f2f4; color:#666666;}"
                "QPushButton:checked{border:1px solid #1565c0;"
                " background:#e3f0fb; color:#1565c0; font-weight:bold;}"
                "QPushButton:disabled{color:#c9cdd2; background:#f4f5f7;}")
        self.unit_group = QButtonGroup(None)   # QObject，不能以 _Row 为父
        self.unit_group.addButton(self.unit_v_btn)
        self.unit_group.addButton(self.unit_a_btn)
        self.unit_group.setExclusive(True)
        self.unit_v_btn.setChecked(True)
        self.unit_v_btn.setToolTip("电压 (V)")
        self.unit_a_btn.setToolTip("电流 (A)")
        self.save_chk = QPushButton("数据保存: 关")
        self.save_chk.setCheckable(True)
        self.save_chk.setCursor(Qt.PointingHandCursor)
        self.save_chk.setStyleSheet(
            "QPushButton{font-size:9pt; font-weight:bold; padding:3px 6px;"
            " border:1px solid #c9cdd2; border-radius:4px;"
            " background:#f1f2f4; color:#666666;}"
            "QPushButton:checked{border:1px solid #1a7f37;"
            " background:#e7f4ea; color:#1a7f37;}")
        self.save_value = False   # 存储的记录开关（期望状态；未使能行暂不生效）
        cfg = QHBoxLayout()
        cfg.setSpacing(2)            # 标签与其输入框紧贴
        cfg.addWidget(QLabel("比例:"))
        cfg.addWidget(self.scale_spin)
        cfg.addSpacing(8)            # 两组之间留组间距
        cfg.addWidget(QLabel("偏移:"))
        cfg.addWidget(self.offset_spin)
        box_v.addLayout(cfg)
        unit_row = QHBoxLayout()
        unit_row.setSpacing(2)
        unit_row.addWidget(QLabel("单位:"))
        unit_row.addWidget(self.unit_v_btn)
        unit_row.addWidget(self.unit_a_btn)
        unit_row.addStretch(1)
        box_v.addLayout(unit_row)
        box_v.addWidget(self.save_chk)

        self.box_proxy = QGraphicsProxyWidget()
        self.box_proxy.setWidget(self.box)

        # ---- 波形区（独立坐标系） ----
        self.plot = pg.PlotItem(
            axisItems={"bottom": pg.DateAxisItem(orientation="bottom")})
        self.plot.showAxis("left")                    # 行内独立电压轴（不显示轴标题）
        self.plot.getAxis("left").setWidth(56)        # 固定轴宽：各行曲线起点对齐
        self.plot.showAxis("bottom")                  # 行内独立时间轴
        self.plot.setLabel("bottom", "时间")
        self.plot.showGrid(x=True, y=True, alpha=0.25)
        self.plot.setMouseEnabled(x=True, y=False)
        self.plot.hideButtons()
        self.plot.vb.setBackgroundColor("w")   # 默认白底；禁用行由 set_enabled 灰化
        self.curve = self.plot.plot([], [], pen=pg.mkPen(color, width=2))

        cross_pen = pg.mkPen(color="#9aa0a6", width=1, style=Qt.DashLine)
        self.vline = pg.InfiniteLine(angle=90, movable=False, pen=cross_pen)
        self.hline = pg.InfiniteLine(angle=0, movable=False, pen=cross_pen)
        self.cursor_label = pg.TextItem(anchor=(0, 1), fill=(255, 255, 255, 230))
        for it in (self.vline, self.hline, self.cursor_label):
            self.plot.addItem(it, ignoreBounds=True)
            it.hide()

        self.y_lo, self.y_hi = 0.0, 1.0
        self.plot.vb.setYRange(self.y_lo, self.y_hi, padding=0)
        self.plot.vb.enableAutoRange(axis=pg.ViewBox.YAxis, enable=False)

        self.t_arr = None
        self.v_arr = None

    def set_enabled(self, on: bool):
        """使能态外观：正常配色 / 整行灰化；未使能时全部配置控件只读"""
        if self.enabled == on:
            return
        self.enabled = on
        self.curve.setPen(pg.mkPen(self.color if on else GRAY_CURVE, width=2))
        self.plot.vb.setBackgroundColor("w" if on else GRAY_BG)
        self.apply_save_state()
        for w in (self.unit_v_btn, self.unit_a_btn,
                  self.scale_spin, self.offset_spin):
            w.setEnabled(on)

    def apply_save_state(self):
        """数据保存按钮：有效状态 = 通道使能 ∧ 记录开关；
        未使能时显示为关且置灰不可点（存储的偏好保留，重新使能即恢复）"""
        effective = self.enabled and self.save_value
        self.save_chk.setEnabled(self.enabled)
        self.save_chk.setChecked(effective)
        self.save_chk.setText("数据保存: 开" if effective else "数据保存: 关")

    def value_at(self, t: float):
        """该通道在时刻 t 最近的采样 (t_i, v_i)；未使能或无数据返回 None"""
        if not self.enabled or self.t_arr is None or self.t_arr.size == 0:
            return None
        i = int(np.searchsorted(self.t_arr, t))
        if i >= self.t_arr.size:
            i = self.t_arr.size - 1
        if i > 0 and abs(t - self.t_arr[i - 1]) < abs(t - self.t_arr[i]):
            i -= 1
        return float(self.t_arr[i]), float(self.v_arr[i])


class AnalyzerView(pg.GraphicsLayoutWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setBackground("w")
        self.rows = {}
        self.hover_t = None
        self.pinned_t = None
        self.config_cb = None      # 内联配置变更回调 (ch, values_dict)
        self._show_placeholder("等待设备配置…")
        self.scene().sigMouseMoved.connect(self._on_mouse_moved)
        self.scene().sigMouseClicked.connect(self._on_mouse_clicked)

    # ---------------- 行管理 ----------------

    def _show_placeholder(self, text: str):
        self._placeholder = pg.LabelItem(justify="center")
        self._placeholder.setText(f'<div style="color:#999">{text}</div>')
        self.addItem(self._placeholder, row=0, col=1)

    def rebuild(self, specs):
        """specs: [(ch, name, unit, y_lo, y_hi, enabled)]，固定四路、按 ch 升序"""
        self.clear()
        self.rows = {}
        self.pinned_t = None
        self.hover_t = None

        if not specs:
            self._show_placeholder("等待设备配置…")
            return

        for r, (ch, name, unit, y_lo, y_hi, enabled) in enumerate(specs):
            color = channel_color(ch)
            row = _Row(ch, color)
            self.rows[ch] = row

            if r > 0:
                row.plot.setXLink(self.rows[specs[r - 1][0]].plot)
            row.y_lo, row.y_hi = y_lo, y_hi
            row.plot.vb.setYRange(y_lo, y_hi, padding=0)   # Y 轴固定为换算后的量程
            row.set_enabled(enabled)
            self._wire_config(row)

            self.addItem(row.box_proxy, row=r, col=0)
            self.addItem(row.plot, row=r, col=1)

        try:
            self.ci.layout.setColumnFixedWidth(0, 240)
        except AttributeError:
            pass
        self._apply_row_heights()

    def _wire_config(self, row):
        """把该行内联控件的变更接到 config_cb 回调"""

        def _on_save_toggled(on: bool, rw=row):
            rw.save_chk.setText("数据保存: 开" if on else "数据保存: 关")
            self._emit_config(rw)

        row.save_chk.toggled.connect(_on_save_toggled)
        for b in (row.unit_v_btn, row.unit_a_btn):
            b.toggled.connect(lambda _on, rw=row: self._emit_config(rw))
        for w in (row.scale_spin, row.offset_spin):
            w.valueChanged.connect(lambda _v, rw=row: self._emit_config(rw))

    def _emit_config(self, row):
        if self.config_cb is None or not row.enabled:
            return   # 未使能通道不存在"保存中"的数据，也不回写偏好
        self.config_cb(row.ch, {
            "save": row.save_chk.isChecked(),
            "unit": "V" if row.unit_v_btn.isChecked() else "A",
            "scale": row.scale_spin.value(),
            "offset": row.offset_spin.value(),
        })

    def set_channel_controls(self, ch, local):
        """把主机侧配置值填入行内控件（不触发回调）"""
        row = self.rows.get(ch)
        if row is None:
            return
        for w, val in ((row.save_chk, local.save),
                       (row.unit_v_btn, local.unit),
                       (row.unit_a_btn, local.unit),
                       (row.scale_spin, local.scale),
                       (row.offset_spin, local.offset)):
            w.blockSignals(True)
            if w is row.save_chk:
                row.save_value = bool(val)
            elif w in (row.unit_v_btn, row.unit_a_btn):
                w.setChecked((val == "V") == (w is row.unit_v_btn))
            else:
                w.setValue(float(val))
            w.blockSignals(False)
        row.apply_save_state()

    def resizeEvent(self, ev):
        """窗口大小变化时按通道数均分行高（延迟到布局之后）"""
        super().resizeEvent(ev)
        QTimer.singleShot(0, self._apply_row_heights)

    def _apply_row_heights(self):
        if not self.rows:
            return
        each = max(110, int(self.size().height() / len(self.rows)) - 8)
        for row in self.rows.values():
            row.plot.setMinimumHeight(each)
            row.plot.setMaximumHeight(each)
            row.box.setFixedSize(240, each)
            # 布局约束必须设在代理件上（设在内嵌 QFrame 上对网格布局无效，
            # 放大→还原后行位置会卡在放大时的排布不回流）
            row.box_proxy.setMinimumHeight(each)
            row.box_proxy.setMaximumHeight(each)
        # 约束变更后强制布局失效并立即重排
        lay = self.ci.layout
        if lay is not None:
            lay.invalidate()
            lay.activate()
        self.ci.updateGeometry()

    def has_rows(self) -> bool:
        return bool(self.rows)

    # ---------------- 数据与标签 ----------------

    def update_curve(self, ch: int, t_arr, v_arr):
        row = self.rows.get(ch)
        if row is None:
            return
        row.t_arr = t_arr
        row.v_arr = v_arr
        row.curve.setData(t_arr, v_arr)
        # 每次数据更新后重申固定量程，杜绝 Y 轴自动缩放
        row.plot.vb.setYRange(row.y_lo, row.y_hi, padding=0)

    def set_label(self, ch: int, name: str, value_text: str, unit: str,
                  y_lo: float, y_hi: float, enabled: bool = True,
                  raw_text: str = "--", volt_text: str = "--"):
        row = self.rows.get(ch)
        if row is None:
            return
        if enabled:
            name_color, value_color, sub_color = "#333333", row.color, "#666666"
            val_bg = "white"
        else:
            name_color = value_color = sub_color = GRAY_TEXT
            val_bg = GRAY_BG
        row.name_label.setText(
            f'<div style="font-size:9pt;color:{name_color}">{name}'
            f' <span style="font-size:8pt;color:#999999">(CH{ch})</span></div>')
        row.value_label.setText(f"{value_text} {unit}")
        qss = VALUE_LABEL_QSS % value_color
        if val_bg != "white":
            qss = qss.replace("background:white", f"background:{val_bg}")
        row.value_label.setStyleSheet(qss)
        row.info_label.setStyleSheet(INFO_LABEL_QSS % (sub_color,))
        row.info_label.setText(
            f'<div style="font-size:8pt;color:{sub_color}">'
            f'量程: {y_lo:g} ~ {y_hi:g} {unit}</div>'
            f'<div style="font-size:8pt;color:{sub_color}">RAW: {raw_text}</div>'
            f'<div style="font-size:8pt;color:{sub_color}">VOLT: {volt_text}</div>')

    def set_y_range(self, ch: int, y_lo: float, y_hi: float):
        """运行中换算参数变化时，同步该行的纵轴量程"""
        row = self.rows.get(ch)
        if row is None:
            return
        row.y_lo, row.y_hi = y_lo, y_hi
        row.plot.vb.setYRange(y_lo, y_hi, padding=0)

    def set_x_range_all(self, t0: float, t1: float):
        for row in self.rows.values():
            row.plot.setXRange(t0, t1, padding=0)

    def clear_data(self):
        for row in self.rows.values():
            row.t_arr = None
            row.v_arr = None
            row.curve.setData([], [])
        self.pinned_t = None
        self.hover_t = None
        self._hide_cursor()

    # ---------------- 全局时间游标 ----------------

    def pin_at(self, t: float):
        self.pinned_t = t
        self._render_cursor(t, pinned=True)

    def clear_pin(self):
        self.pinned_t = None
        if self.hover_t is not None:
            self._render_cursor(self.hover_t, pinned=False)
        else:
            self._hide_cursor()

    def _hide_cursor(self):
        for row in self.rows.values():
            row.vline.hide()
            row.hline.hide()
            row.cursor_label.hide()

    def _render_cursor(self, t: float, pinned: bool):
        ts = datetime.fromtimestamp(t).strftime("%H:%M:%S")
        for row in self.rows.values():
            row.vline.setPos(t)
            row.vline.show()
            pt = row.value_at(t)   # 未使能行返回 None，只画竖线
            if pt is None:
                row.hline.hide()
                row.cursor_label.hide()
                continue
            vi = pt[1]
            row.hline.setPos(vi)
            row.hline.show()
            row.cursor_label.setText(f"{ts}  {vi:.4f}")
            row.cursor_label.setPos(t, vi)
            row.cursor_label.show()

    def _on_mouse_moved(self, scene_pos):
        hit = None
        for row in self.rows.values():
            if row.plot.vb.sceneBoundingRect().contains(scene_pos):
                hit = row
                break
        if hit is None:
            self.hover_t = None
            if self.pinned_t is not None:
                self._render_cursor(self.pinned_t, pinned=True)
            else:
                self._hide_cursor()
            return
        view_x = hit.plot.vb.mapSceneToView(scene_pos).x()
        pt = hit.value_at(view_x)
        if pt is None:
            self.hover_t = None
            self._hide_cursor()
            return
        self.hover_t = pt[0]
        self._render_cursor(pt[0], pinned=False)

    def _on_mouse_clicked(self, evt):
        if evt.button() != Qt.LeftButton or not self.rows:
            return
        for row in self.rows.values():
            if row.plot.vb.sceneBoundingRect().contains(evt.scenePos()):
                view_x = row.plot.vb.mapSceneToView(evt.scenePos()).x()
                pt = row.value_at(view_x)
                if pt is not None:
                    self.pin_at(pt[0])
                return
