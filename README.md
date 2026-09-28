# ADS1115 四路分时复用驱动

16 位 ΔΣ ADC 的平台无关驱动。核心层是纯 C，与 ESP-IDF 完全解耦；
ESP32 只作为**可替换的适配层**存在。

- 四路分时复用：单次转换轮询，逐路独立配置
- 每路量程（PGA）、数据率（DR）、输入选择（MUX）三者完全独立
- 四路可全开、可任意组合、可只留一路，运行期随时切换
- 配置由 `ads1115_chan_cfg_t` 结构体描述，用结构体数组统一管理
- 核心层零依赖，可原样复用到 STM32 / Linux / Arduino
- 一份代码同时支持 ESP32 / S2 / S3 / C2 / C3 / C6 / H2 / P4
- 无动态内存，全部静态分配

---

## 目录结构

```
ads1115-driver/
├── components/
│   ├── ads1115/                     核心驱动：纯 C，零依赖
│   │   ├── include/ads1115.h        全部类型与 API
│   │   ├── ads1115.c                分时轮询状态机
│   │   └── CMakeLists.txt           刻意不写任何 REQUIRES
│   └── ads1115_esp32/               ESP-IDF 适配层（可选件）
│       ├── include/ads1115_esp32.h
│       ├── ads1115_esp32.c          i2c_master + esp_timer
│       └── CMakeLists.txt
├── main/                            四路全开示例 + WiFi 遥测（工程主目录）
│   ├── main.c                       组装层：采样循环 + 命令应用（驱动唯一写者）
│   ├── ads1115_proto.c/.h           JSON 行协议 v2（帧解析/构建，纯逻辑）
│   ├── ads1115_cfg.c/.h             通道配置模型 + NVS 持久化 + 请求队列
│   ├── ads1115_net.c/.h             WiFi STA + mDNS + TCP + 换网回滚
│   └── Kconfig.projbuild            WiFi/上位机参数（idf.py menuconfig 可改）
├── Tools/                           Python 上位机 + 打包产物 + 版本留档（见 Tools/README.md）
├── CMakeLists.txt                   ESP-IDF 工程入口（仓库根即工程根）
├── sdkconfig.defaults
└── test/
    └── host/                        宿主机单元测试（不需要硬件）
        ├── test_ads1115.c
        └── Makefile
```

---

## 分层架构

```
┌────────────────────────────────────────────────────┐
│  应用层      app_main / 业务逻辑                    │  你的代码
├────────────────────────────────────────────────────┤
│  适配层      ads1115_esp32.c                       │  唯一认识 ESP-IDF 的地方
│              i2c_master / esp_timer / vTaskDelay   │
├────────────────────────────────────────────────────┤
│  Port 接口   ads1115_port_t（3 个函数指针）         │  解耦边界
├────────────────────────────────────────────────────┤
│  核心层      ads1115.c / ads1115.h                 │  纯 C，零依赖，跨平台
│              分时轮询状态机 + 配置结构体数组        │
└────────────────────────────────────────────────────┘
```

核心层 `ads1115.c` 只 include `<stdint.h> <stddef.h> <stdbool.h> <string.h>`，
连 `esp_err_t` 都不认识。证据是 `test/host/` 下的测试：把 I²C 换成内存里的
假寄存器，就能直接在 PC 上把全部逻辑跑通。

---

## 快速开始（ESP-IDF）

```bash
# 仓库根目录即 ESP-IDF 工程根，直接在根目录执行：
idf.py set-target esp32s3        # 或 esp32 / esp32c3 / esp32c6 ...
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

初始化只有三步：

```c
static ads1115_esp32_ctx_t io_ctx;
static ads1115_t adc;

io_ctx.scl_speed_hz = 400000;
io_ctx.timeout_ms   = 100;

ads1115_esp32_attach(&adc, bus, &io_ctx, ADS1115_I2C_ADDR_MIN);  /* 0x48 */
ads1115_set_channels(&adc, s_chan_cfg, ADS1115_CHANNEL_COUNT);   /* 批量下发 */
```

主循环里周期推进状态机，采样由驱动内部自动轮转：

```c
while (true) {
    ads1115_poll(&adc);                    /* 建议 1–10 ms 调用一次 */
    vTaskDelay(pdMS_TO_TICKS(5));
}
```

---

## 宿主机测试

不需要任何硬件、也不需要 ESP-IDF：

```bash
cd test/host
make && ./test_ads1115      # 常规环境
make xcode                  # 若宿主机 clang 与 CommandLineTools SDK 版本不匹配
```

覆盖 63 项断言：配置字拼装、四路全开轮询、只启用一路、逐路量程独立性、
参数边界、I²C 故障恢复、阻塞读取与运行期改量程。

---

## 配置管理

每路一份 `ads1115_chan_cfg_t`，用数组统一持有：

```c
typedef struct {
    ads1115_mux_t mux;      /* 输入选择：单端 / 差分 */
    ads1115_pga_t pga;      /* 量程 */
    ads1115_dr_t  dr;       /* 数据率 */
    bool          enabled;  /* 是否参与轮询 */
    float         scale;    /* 工程换算：value = volt * scale + offset */
    float         offset;
} ads1115_chan_cfg_t;
```

实例内部：

```c
typedef struct {
    ads1115_port_t     port;
    uint8_t            i2c_addr;
    bool               inited;

    ads1115_chan_cfg_t ch[ADS1115_CHANNEL_COUNT];      /* 配置数组 */
    uint8_t            enabled_mask;                   /* 使能位图 */

    ads1115_result_t   result[ADS1115_CHANNEL_COUNT];  /* 结果缓存 */
    /* ... 轮询运行时状态 ... */
} ads1115_t;
```

### 运行时切换

```c
ads1115_set_mask(&adc, 0x0F);                    /* 四路全开 */
ads1115_set_mask(&adc, 1u << 0);                 /* 只留第 0 路 */
ads1115_set_mask(&adc, (1u << 0) | (1u << 2));   /* 任意组合 */
ads1115_set_enabled(&adc, 1u, false);            /* 单独关掉某路 */

/* 临时改某一路的量程 */
ads1115_chan_cfg_t c = *ads1115_get_config(&adc, 1u);
c.pga = ADS1115_PGA_2V048;
ads1115_set_channel(&adc, 1u, &c);
```

### 结果读取

```c
const ads1115_result_t *r = ads1115_get_result(&adc, 0);
if (r->valid) {
    /* r->raw   原始有符号码值
       r->volt  换算电压
       r->value 工程值（已套用 scale/offset）
       r->seq   刷新序号，用于判断是否是新数据 */
}
```

需要立即取值而不等轮询时，用阻塞接口：

```c
ads1115_result_t out;
ads1115_read_blocking(&adc, 0u, &out);
```

---

## 关键设计决策

### 为什么必须用单次转换模式

ADS1115 内部只有**一路** ΔΣ 调制器和一个输入 MUX，四路输入本质串行。
驱动每次采样都完整重写配置寄存器（含 MUX/PGA/DR），因此：

- 每路量程可以完全不同，互不干扰
- 连续转换模式做不到这一点——切换 PGA/MUX 会打断正在进行的转换

代价是每次切换有一次转换建立时间，轮询一圈的周期 =
各通道转换耗时之和。

### 为什么用函数指针而不是直接调 IDF

```c
typedef struct {
    ads1115_i2c_transfer_fn transfer;  /* 必填 */
    ads1115_delay_ms_fn     delay_ms;  /* 阻塞读取需要 */
    ads1115_now_ms_fn       now_ms;    /* 非阻塞轮询需要 */
    void                   *ctx;       /* 平台上下文 */
} ads1115_port_t;
```

这样核心层编译时不需要任何平台头文件，也就能被宿主机测试直接编译。
移植到新平台 = 实现一个 `transfer` 回调，不改动核心层一行代码。

### 两套采样接口

| 接口 | 特性 | 适用 |
|---|---|---|
| `ads1115_poll` | 非阻塞，状态机分片，四路自动轮转 | 常规主循环 / 任务 |
| `ads1115_read_blocking` | 阻塞一个转换周期，可指定通道 | 上电自检、事件触发取值 |

两者共用同一份配置数组，可以混用。

---

## 各 ESP32 系列的引脚

驱动本身与芯片无关，唯一有差异的是 I²C 引脚，写在应用层：

| 芯片 | SDA | SCL | 备注 |
|---|---|---|---|
| ESP32 | GPIO21 | GPIO22 | 经典款默认 |
| ESP32-S2 / S3 / C3 | GPIO8 | GPIO9 | 示例默认值 |
| ESP32-C6 / H2 | GPIO6 | GPIO7 | 按开发板实际调整 |

适配层用的 `i2c_master` 是 ESP-IDF 通用外设 API，全部 ESP32 系列接口一致，
因此 `ads1115_esp32.c` 里没有任何 `CONFIG_IDF_TARGET_*` 条件编译。

---

## 移植到其他平台

三步，核心层不动：

1. 实现一个 transfer 回调：

```c
static ads1115_err_t my_transfer(void *ctx, uint8_t addr,
                                 const uint8_t *tx, size_t tx_len,
                                 uint8_t *rx, size_t rx_len)
{
    if (rx && rx_len) {
        return HAL_I2C_Mem_Read(&hi2c1, addr << 1, tx[0],
                                I2C_MEMADD_SIZE_8BIT, rx, rx_len, 100)
               == HAL_OK ? ADS1115_OK : ADS1115_ERR_IO;
    }
    return HAL_I2C_Mem_Write(&hi2c1, addr << 1, tx[0],
                             I2C_MEMADD_SIZE_8BIT, (uint8_t *)&tx[1], tx_len - 1, 100)
           == HAL_OK ? ADS1115_OK : ADS1115_ERR_IO;
}
```

2. 填 `ads1115_port_t`，调 `ads1115_init`。
3. 周期调用 `ads1115_poll`（或只用阻塞接口就不需要 `now_ms`）。

---

## 硬件注意事项

**引脚分配**：ADS1115 只有 AIN0–AIN3 四个物理输入。差分会占掉两个引脚
（如 `AIN0_AIN1`），此时就不能再对 AIN0 / AIN1 配单端，否则 MUX 互相抢占。
驱动不会替你检查这种冲突，配置阶段需自行保证。

**输入绝对上限**：无论选哪档 PGA，AIN 引脚电压都不得超过 VDD + 0.3 V。
3.3 V 供电时上限约 3.6 V，选 ±6.144 V 档也不代表能输入 6 V。

**低边电流采样**：采样电阻两端用开尔文连线引出，不要与大电流共用铜皮；
在 ADC 输入端加 RC 抗混叠（100 Ω + 100 nF，fc ≈ 16 kHz），
数据率取 8–128 SPS 可借用 ΔΣ 的 50/60 Hz 陷波。

**上拉电阻**：模块自带的 10 kΩ 在长线或 400 kHz 下偏大，建议换 2.2–4.7 kΩ。

**I2C 地址**：ADDR 接 GND=0x48，VDD=0x49，SDA=0x4A，SCL=0x4B。

---

## WiFi 实时上位机（可选）

设备端已集成 WiFi 遥测（`main/ads1115_net.c`，协议 v2）：连上 WiFi 后自动通过
mDNS 发现局域网内的上位机，逐采样推送 **raw + 引脚电压**（工程值换算在上位机）；
同时支持上位机远程配置——四路通道（MUX/PGA/数据率/使能，NVS 断电保存）、
WiFi 账号（带回滚）。上电按住 BOOT 键可恢复出厂。

1. 电脑端：双击 `Tools/dist/ADS1115Host.exe`（逻辑分析仪式界面：每个使能通道
   一行，左侧通道名与实时值、右侧波形；CSV 记录；设备配置面板）。首次运行需
   允许防火墙放行 TCP 9000 与 UDP 5353，详见 `Tools/README.md`。
2. 设备端：`idf.py menuconfig` → "ADS1115 Demo Configuration" 配置 WiFi
   SSID/密码与上位机名字/端口（另有固定 IP 后备）。
3. 构建烧录：`idf.py build flash monitor`。

**版本配对**：上位机与固件须同代升级（当前 v2）。已发布版本成对归档在
`Tools/releases/`。断线后设备每 3 秒自动重连，网络故障不影响本地采样。

---

## API 速查

| 函数 | 说明 |
|---|---|
| `ads1115_init` | 初始化实例，全通道禁用 |
| `ads1115_set_channel` | 覆盖单路配置 |
| `ads1115_set_channels` | 批量覆盖配置数组 |
| `ads1115_set_enabled` | 运行期开关单路 |
| `ads1115_set_mask` | 一次性设置使能位图 |
| `ads1115_get_config` | 读配置（只读视图） |
| `ads1115_get_result` | 读最近结果（只读视图） |
| `ads1115_poll` | 非阻塞推进轮询状态机 |
| `ads1115_read_blocking` | 阻塞式单次读取 |
| `ads1115_pga_fsr` | 量程 → 满量程电压 |
| `ads1115_dr_sps` | 档位 → SPS |
| `ads1115_conv_time_ms` | 档位 → 转换耗时（含余量） |
| `ads1115_make_config` | 配置结构体 → 16 位配置字 |
| `ads1115_esp32_attach` | （适配层）绑定 I²C 总线并初始化 |
| `ads1115_esp32_detach` | （适配层）摘除设备 |
