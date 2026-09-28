#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
本地配置存储：换算与显示属性（通道名/单位/小数位/scale/offset/CSV 开关）。

文件位置：与 exe（或脚本）同级的 config/host_config.json。
这些配置属于主机侧，与设备无关；设备只提供 raw 与 volt。
"""

import json
import sys
from dataclasses import dataclass, asdict, field
from pathlib import Path


@dataclass
class ChannelLocal:
    name: str = "CH?"
    unit: str = "V"
    decimals: int = 3
    scale: float = 1.0     # 真实值 = volt * scale + offset
    offset: float = 0.0
    save: bool = True      # 记录 CSV 的用户开关（前提：通道已使能）


def app_base_dir() -> Path:
    """exe 所在目录（打包后）或本文件上级目录（源码运行）"""
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parent.parent


CONFIG_DIR = app_base_dir() / "config"
CONFIG_FILE = CONFIG_DIR / "host_config.json"


def default_channels() -> dict:
    """出厂本地默认：纯 ADC 电压测量，四路均为 V"""
    return {
        0: ChannelLocal(name="CH0 电压", unit="V", decimals=3,
                        scale=1.0, offset=0.0, save=True),
        1: ChannelLocal(name="电池电压", unit="V", decimals=3,
                        scale=3.0, offset=0.0, save=True),
        2: ChannelLocal(name="CH2 电压", unit="V", decimals=3,
                        scale=1.0, offset=0.0, save=True),
        3: ChannelLocal(name="CH3 电压", unit="V", decimals=3,
                        scale=1.0, offset=0.0, save=True),
    }


class AppStore:
    """config/host_config.json 的读写；所有字段带默认值兜底"""

    def __init__(self):
        self.channels: dict = {}
        self.load()

    def load(self):
        self.channels = default_channels()
        try:
            data = json.loads(CONFIG_FILE.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return
        for key, item in (data.get("channels") or {}).items():
            try:
                ch = int(key)
            except (TypeError, ValueError):
                continue
            base = self.channels.get(ch, ChannelLocal())
            self.channels[ch] = ChannelLocal(
                name=str(item.get("name", base.name)),
                unit=str(item.get("unit", base.unit)),
                decimals=int(item.get("decimals", base.decimals)),
                scale=float(item.get("scale", base.scale)),
                offset=float(item.get("offset", base.offset)),
                save=bool(item.get("save", base.save)),
            )

    def save(self):
        try:
            CONFIG_DIR.mkdir(exist_ok=True)
            payload = {"channels": {str(ch): asdict(c)
                                    for ch, c in self.channels.items()}}
            CONFIG_FILE.write_text(
                json.dumps(payload, ensure_ascii=False, indent=2),
                encoding="utf-8")
        except OSError:
            pass

    def conversion(self, ch: int) -> ChannelLocal:
        return self.channels.get(ch, ChannelLocal())
