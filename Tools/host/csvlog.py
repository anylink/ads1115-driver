#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
CSV 记录器：只负责"写"，不负责"该不该写"。

通道过滤（使能 ∧ 用户开关）由调用方判定后调用 write_row；
这样本模块保持纯粹的文件 I/O，方便测试。
"""

import csv
from datetime import datetime
from pathlib import Path

HEADER = ["host_time", "dev_ms", "channel", "ch_name", "raw", "volt", "value", "seq"]


class CsvRecorder:
    def __init__(self):
        self._file = None
        self._writer = None
        self.rows = 0
        self.path: Path | None = None

    @property
    def recording(self) -> bool:
        return self._file is not None

    def start(self, out_dir: Path) -> Path:
        out_dir.mkdir(exist_ok=True)
        self.path = out_dir / f"ads1115_{datetime.now():%Y%m%d_%H%M%S}.csv"
        self._file = open(self.path, "w", newline="", encoding="utf-8-sig")
        self._writer = csv.writer(self._file)
        self._writer.writerow(HEADER)
        self._file.flush()
        self.rows = 0
        return self.path

    def write_row(self, dev_ms: int, ch: int, ch_name: str,
                  raw: int, volt: float, value: float, seq: int):
        if self._writer is None:
            return
        dt = datetime.now()
        self._writer.writerow([
            f"{dt:%Y-%m-%d %H:%M:%S}.{dt.microsecond // 1000:03d}",
            dev_ms, ch, ch_name, raw, f"{volt:.6f}", f"{value:.6f}", seq,
        ])
        self.rows += 1
        if self.rows % 50 == 0:
            self._file.flush()

    def stop(self):
        if self._file is not None:
            self._file.flush()
            self._file.close()
        self._file = None
        self._writer = None
