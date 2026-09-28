#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
JSON 行协议（v2）—— 与设备端 main/ads1115_proto.h 严格对应。

帧都是一行 JSON。方向：
  设备→主机：hello / 采样（无 type 字段）/ ack / event
  主机→设备：set_ch / set_net / reset_cfg / get_cfg

数值含义与驱动位域一致：
  mux 0..7（0..3 差分，4..7 单端 AIN0..AIN3 对 GND）
  pga 0..5（±6.144 ... ±0.256 V）
  dr  0..7（8 ... 860 SPS）
"""

import json
from dataclasses import dataclass

PGA_FSR = (6.144, 4.096, 2.048, 1.024, 0.512, 0.256)
DR_SPS = (8, 16, 32, 64, 128, 250, 475, 860)

MUX_OPTIONS = [
    ("AIN0−AIN1 差分", 0), ("AIN0−AIN3 差分", 1),
    ("AIN1−AIN3 差分", 2), ("AIN2−AIN3 差分", 3),
    ("AIN0−GND", 4), ("AIN1−GND", 5), ("AIN2−GND", 6), ("AIN3−GND", 7),
]
PGA_OPTIONS = [
    ("±6.144 V", 0), ("±4.096 V", 1), ("±2.048 V", 2),
    ("±1.024 V", 3), ("±0.512 V", 4), ("±0.256 V", 5),
]
DR_OPTIONS = [
    ("8 SPS", 0), ("16 SPS", 1), ("32 SPS", 2), ("64 SPS", 3),
    ("128 SPS", 4), ("250 SPS", 5), ("475 SPS", 6), ("860 SPS", 7),
]


@dataclass(frozen=True)
class Sample:
    dev_ms: int
    ch: int
    raw: int
    volt: float
    seq: int


def parse_frame(line: bytes):
    """一行 JSON → dict；坏帧返回 None"""
    try:
        obj = json.loads(line.decode("utf-8", "replace"))
        return obj if isinstance(obj, dict) else None
    except (ValueError, TypeError):
        return None


def make_sample(frame: dict):
    """采样帧（无 type 字段）→ Sample；字段不合法返回 None"""
    if "type" in frame:
        return None
    try:
        return Sample(
            dev_ms=int(frame["dev_ms"]),
            ch=int(frame["ch"]),
            raw=int(frame["raw"]),
            volt=float(frame["volt"]),
            seq=int(frame["seq"]),
        )
    except (KeyError, ValueError, TypeError):
        return None


def mux_pins(mux: int):
    """该 MUX 占用的物理引脚集合（用于 UI 端冲突校验）"""
    if mux <= 3:
        pairs = ((0, 1), (0, 3), (1, 3), (2, 3))
        return set(pairs[mux])
    return {mux - 4}


def build_set_ch(ch: int, mux: int, pga: int, dr: int, enabled: bool) -> str:
    return (f'{{"cmd":"set_ch","ch":{ch},"mux":{mux},"pga":{pga},'
            f'"dr":{dr},"enabled":{"true" if enabled else "false"}}}')


def build_set_net(ssid: str, password: str) -> str:
    return json.dumps({"cmd": "set_net", "ssid": ssid, "pass": password},
                      ensure_ascii=False)


def build_reset_cfg() -> str:
    return '{"cmd":"reset_cfg"}'


def build_get_cfg() -> str:
    return '{"cmd":"get_cfg"}'


def build_ping() -> str:
    """hello 回执：设备收到任意已知命令即停止补发 hello"""
    return '{"cmd":"ping"}'
