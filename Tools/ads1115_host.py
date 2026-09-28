#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ADS1115 上位机入口（实现在 host/ 包，见 host/app.py）。

用法：
    ADS1115Host.exe
    或  .venv\\Scripts\\python.exe ads1115_host.py [--port 9000] [--smoke]
"""

import sys

from host.app import main

if __name__ == "__main__":
    sys.exit(main())
