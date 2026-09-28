# ADS1115 上位机（v2）

接收 ESP32 设备通过 WiFi 推送的采样数据，并可**远程配置设备**：逻辑分析仪式
多通道显示 + 实时曲线 + 带时间戳的 CSV 记录。

> 版本配对：v2 上位机 ↔ v2 固件（协议 v2，双向命令）。旧版本归档在
> `releases/` 下，仅供回退整个环境使用。

## 目录结构

```
Tools/
├── ads1115_host.py        入口（实现在 host/ 包）
├── host/                  上位机模块
│   ├── app.py             主窗口组装（业务状态唯一所有者）
│   ├── analyzer_view.py   逻辑分析仪视图（每通道一行 + 全局时间游标）
│   ├── config_dialog.py   设备配置 / 网络配置对话框（含 MUX 冲突校验）
│   ├── net.py             TCP 服务端 + mDNS 广告 + 网卡枚举
│   ├── protocol.py        JSON 行协议（v2）定义与构建
│   ├── storage.py         本地换算配置（config/host_config.json）
│   └── csvlog.py          CSV 记录器
├── mock_device.py         模拟设备（无硬件联调/演示用）
├── dist/ADS1115Host.exe   单文件可执行
└── releases/              已发布版本留档（勿删）
```

## 使用

**直接运行** `dist\ADS1115Host.exe`；或源码方式：

```bat
python -m venv .venv
.venv\Scripts\python.exe -m pip install -r requirements.txt
start_host.bat
```

首次运行防火墙放行：TCP 9000（数据）+ UDP 5353（mDNS）。

## 能配置什么

**设备侧**（下发到设备，NVS 断电保存）：
- 每路 MUX（单端/差分）、PGA（测量范围）、数据率（8~860 SPS）、使能开关
- WiFi SSID / 密码（约 60 秒连不上自动回滚旧配置）
- 恢复出厂：上电按住 BOOT 键清除 NVS

**主机侧**（存于 `config/host_config.json`，与设备无关）：
- 每路通道名、单位、小数位、scale/offset（真实值 = volt×scale+offset）
- 每路 CSV 记录开关（默认开）

MUX 冲突（同一物理引脚被两个使能通道占用）在配置面板校验拦截。

## 数据与显示规则

- **CSV 保留条件 = 通道使能 ∧ 该路记录开关**；单文件、表头固定、按通道过滤行；
  列：`host_time, dev_ms, channel, ch_name, raw, volt, value, seq`
- **显示 = 四行常驻**：未使能的通道整行灰化（标签 --、灰底、无波形），重新使能即恢复彩色；禁用行不参与时间游标取值
- 顶栏"窗口"为曲线时间跨度（30~600 s）；"冻结"暂停显示（记录不受影响）

## 协议（v2）

每帧一行 JSON。设备连接后先推 hello（当前配置），随后逐采样推送：

```
{"dev_ms":123456,"ch":1,"raw":21223,"volt":1.327000,"seq":1234}
```

主机→设备命令：`set_ch` / `set_net` / `reset_cfg` / `get_cfg`，设备逐条应答 ack。
设备只输出 raw 与 volt，工程值换算在上位机完成。

## 无硬件联调

```bat
.venv\Scripts\python.exe mock_device.py --host 127.0.0.1 --port 9000
```

模拟设备发送 hello + 模拟采样（CH1 电池 8SPS、CH2 正弦 16SPS），
应答全部命令，可用于界面演示与协议回归。

## 重新打包 / 改图标 / 留档

```bat
.venv\Scripts\python.exe make_icon.py
.venv\Scripts\pyinstaller --noconfirm --onefile --windowed ^
    --icon app.ico --add-data "app.ico;." --name ADS1115Host ads1115_host.py
```

**发布新版本前必须先归档**：把当期 exe、host_src、固件源码拷入
`releases/<日期>_<版本>/`（与 v1/v2 目录同格式），再覆盖 dist。
