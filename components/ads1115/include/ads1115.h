/**
 * @file ads1115.h
 * @brief ADS1115 16 位 4 通道 ADC 驱动 —— 纯 C 核心层
 *
 * 设计约束（重要）：
 *   - 本文件与任何 MCU / RTOS / 框架零耦合，只依赖 C 标准库头文件。
 *   - 所有硬件访问通过 ads1115_port_t 里的函数指针注入，
 *     移植到新平台只需实现一个 transfer 回调，不改动本文件与 ads1115.c。
 *
 * 分时复用模型：
 *   ADS1115 内部只有一路 ΔΣ 调制器 + 一个输入 MUX，四路输入本质上是串行采样。
 *   本驱动采用【单次转换（single-shot）】模式逐个通道轮询：
 *     写配置(含 MUX/PGA/DR) -> 等待转换完成 -> 读结果 -> 下一路
 *   这样每一路都拥有完全独立的量程(PGA)、数据率(DR)和输入选择(MUX)。
 *   若改用连续转换模式，切换 PGA/MUX 会打断正在进行的转换，无法做到逐路独立配置。
 */

#ifndef ADS1115_H
#define ADS1115_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 常量                                                               */
/* ------------------------------------------------------------------ */

/** 逻辑通道槽位数量：与配置数组长度一致，每槽可独立指定任意 MUX 选项 */
#define ADS1115_CHANNEL_COUNT 4u

/** I2C 从机地址范围，由 ADDR 引脚决定：GND=0x48, VDD=0x49, SDA=0x4A, SCL=0x4B */
#define ADS1115_I2C_ADDR_MIN  0x48u
#define ADS1115_I2C_ADDR_MAX  0x4Bu

/** 寄存器地址 */
#define ADS1115_REG_CONVERSION 0x00u
#define ADS1115_REG_CONFIG     0x01u
#define ADS1115_REG_LO_THRESH  0x02u
#define ADS1115_REG_HI_THRESH  0x03u

/** 反相/同相阈值寄存器互换用的 magic 值（写这两个值可交换比较器极性） */
#define ADS1115_THRESH_ACTIVE_LOW  0x8000u
#define ADS1115_THRESH_ACTIVE_HIGH 0x0000u

/** 转换完成预留余量 (ms)：数据率 860SPS 时 1.16ms，加上 I2C 往返开销 */
#define ADS1115_CONV_MARGIN_MS 2u

/* ------------------------------------------------------------------ */
/* 返回值                                                             */
/* ------------------------------------------------------------------ */

typedef enum {
    ADS1115_OK            = 0,   /**< 成功 */
    ADS1115_ERR_PARAM     = -1,  /**< 参数非法（空指针、越界、非法枚举） */
    ADS1115_ERR_IO        = -2,  /**< I2C 传输失败（NACK / 超时 / 仲裁丢失） */
    ADS1115_ERR_NO_ENABLED = -3, /**< 没有任何通道处于使能状态 */
    ADS1115_ERR_STATE     = -4,  /**< 状态机不允许该操作 */
} ads1115_err_t;

/* ------------------------------------------------------------------ */
/* 枚举：量程 / 数据率 / 输入选择                                      */
/* ------------------------------------------------------------------ */

/** PGA 满量程档位，数值即配置寄存器 PGA[2:0] 位域 */
typedef enum {
    ADS1115_PGA_6V144 = 0x00, /**< ±6.144 V, LSB=187.5  uV */
    ADS1115_PGA_4V096 = 0x01, /**< ±4.096 V, LSB=125.0  uV */
    ADS1115_PGA_2V048 = 0x02, /**< ±2.048 V, LSB=62.5   uV (芯片上电默认) */
    ADS1115_PGA_1V024 = 0x03, /**< ±1.024 V, LSB=31.25  uV */
    ADS1115_PGA_0V512 = 0x04, /**< ±0.512 V, LSB=15.625 uV */
    ADS1115_PGA_0V256 = 0x05, /**< ±0.256 V, LSB=7.8125 uV */
} ads1115_pga_t;

/** 数据率，数值即配置寄存器 DR[2:0] 位域；越低噪声越小 */
typedef enum {
    ADS1115_DR_8SPS   = 0x00,
    ADS1115_DR_16SPS  = 0x01,
    ADS1115_DR_32SPS  = 0x02,
    ADS1115_DR_64SPS  = 0x03,
    ADS1115_DR_128SPS = 0x04, /**< 上电默认，速度/噪声折中 */
    ADS1115_DR_250SPS = 0x05,
    ADS1115_DR_475SPS = 0x06,
    ADS1115_DR_860SPS = 0x07,
} ads1115_dr_t;

/** 输入 MUX 选择，数值即配置寄存器 MUX[2:0] 位域 */
typedef enum {
    ADS1115_MUX_AIN0_AIN1 = 0x00, /**< 差分 AIN0(+)/AIN1(-) */
    ADS1115_MUX_AIN0_AIN3 = 0x01, /**< 差分 AIN0(+)/AIN3(-) */
    ADS1115_MUX_AIN1_AIN3 = 0x02, /**< 差分 AIN1(+)/AIN3(-) */
    ADS1115_MUX_AIN2_AIN3 = 0x03, /**< 差分 AIN2(+)/AIN3(-) */
    ADS1115_MUX_AIN0_GND  = 0x04, /**< 单端 AIN0 对 GND */
    ADS1115_MUX_AIN1_GND  = 0x05, /**< 单端 AIN1 对 GND */
    ADS1115_MUX_AIN2_GND  = 0x06, /**< 单端 AIN2 对 GND */
    ADS1115_MUX_AIN3_GND  = 0x07, /**< 单端 AIN3 对 GND */
} ads1115_mux_t;

/* ------------------------------------------------------------------ */
/* 平台抽象层（Port Layer）                                            */
/* ------------------------------------------------------------------ */

/**
 * @brief I2C 带寄存器指针的读事务回调
 *
 * 语义等价于「重复起始条件」事务：
 *   1. 发送 tx[0..tx_len-1]（对 ADS1115 就是 1 字节寄存器地址）
 *   2. 若 rx != NULL 且 rx_len > 0，在【不释放总线】的前提下继续读取 rx_len 字节
 *   3. 若 rx == NULL 或 rx_len == 0，则本事务为纯写
 *
 * 移植提示：
 *   ESP-IDF  -> i2c_master_transmit_receive() / i2c_master_transmit()
 *   Linux    -> ioctl(I2C_RDWR) 两个 i2c_msg（带 I2C_M_NOSTART 的那个）
 *   STM32    -> HAL_I2C_Mem_Read() / HAL_I2C_Mem_Write()
 *   Arduino  -> Wire.beginTransmission() + requestFrom()
 *
 * @param ctx       平台上下文，由 port.ctx 传入
 * @param dev_addr  7 位从机地址（若平台已在 handle 里绑定地址，可忽略）
 * @return ADS1115_OK 或 ADS1115_ERR_IO
 */
typedef ads1115_err_t (*ads1115_i2c_transfer_fn)(void *ctx,
                                                 uint8_t dev_addr,
                                                 const uint8_t *tx, size_t tx_len,
                                                 uint8_t *rx, size_t rx_len);

/** 毫秒级延时回调（阻塞式读取路径需要） */
typedef void (*ads1115_delay_ms_fn)(void *ctx, uint32_t ms);

/** 单调递增毫秒计数回调（非阻塞轮询路径需要），要求回绕安全 */
typedef uint32_t (*ads1115_now_ms_fn)(void *ctx);

/** 平台抽象层：一次性注入设备句柄，之后核心层不再接触具体框架 */
typedef struct {
    ads1115_i2c_transfer_fn transfer; /**< 必填 */
    ads1115_delay_ms_fn     delay_ms; /**< 阻塞读取需要；纯轮询可为 NULL */
    ads1115_now_ms_fn       now_ms;   /**< 非阻塞轮询需要；为 NULL 时退化为阻塞等待 */
    void                   *ctx;      /**< 平台私有上下文（如 IDF 的 dev handle） */
} ads1115_port_t;

/* ------------------------------------------------------------------ */
/* 配置与结果                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief 单通道配置
 *
 * 每个逻辑通道持有自己独立的一份，互不影响：
 * 量程(PGA)、数据率(DR)、输入选择(MUX) 三者均逐路独立。
 */
typedef struct {
    ads1115_mux_t mux;      /**< 输入选择：单端或差分 */
    ads1115_pga_t pga;      /**< 满量程档位（量程） */
    ads1115_dr_t  dr;       /**< 数据率，决定转换耗时与噪声 */
    bool          enabled;  /**< 是否参与分时轮询 */
    float         scale;    /**< 工程换算系数：value = volt * scale + offset */
    float         offset;   /**< 工程零点偏移 */
} ads1115_chan_cfg_t;

/** 单通道采样结果 */
typedef struct {
    int16_t  raw;    /**< 原始有符号 16 位码值 */
    float    volt;   /**< 换算电压 = raw * FSR / 32768 */
    float    value;  /**< 工程值 = volt * scale + offset */
    uint32_t seq;    /**< 该通道结果的新旧序号，用于判断是否刷新 */
    bool     valid;  /**< 是否已至少完成一次有效采样 */
} ads1115_result_t;

/** 轮询状态机的内部阶段 */
typedef enum {
    ADS1115_STATE_IDLE = 0, /**< 空闲，可以发起下一路转换 */
    ADS1115_STATE_WAIT = 1, /**< 转换进行中，等待建立时间到 */
} ads1115_state_t;

/* ------------------------------------------------------------------ */
/* 设备句柄                                                            */
/* ------------------------------------------------------------------ */

/**
 * @brief 设备实例，可静态分配（无动态内存，嵌入式友好）
 *
 * 配置管理方式：ads1115_chan_cfg_t 结构体数组 ch[]，下标即逻辑通道号。
 */
typedef struct {
    ads1115_port_t     port;        /**< 平台抽象层 */
    uint8_t            i2c_addr;    /**< 7 位 I2C 地址 */
    bool               inited;      /**< 是否已初始化 */

    ads1115_chan_cfg_t ch[ADS1115_CHANNEL_COUNT];     /**< 配置数组 */
    uint8_t            enabled_mask;                  /**< 使能位图，bit n = 通道 n 使能 */

    ads1115_result_t   result[ADS1115_CHANNEL_COUNT]; /**< 结果缓存 */
    uint32_t           seq;                           /**< 全局结果序号 */

    ads1115_state_t    state;       /**< 轮询状态机阶段 */
    uint8_t            cursor;      /**< 上一次发起转换的通道，用于轮转 */
    uint8_t            active;      /**< 当前正在采样的通道 */
    uint32_t           wait_until;  /**< 本次转换最早可读的时刻 (ms) */
    uint32_t           wait_span;   /**< 本次转换需要的毫秒数，供退化阻塞路径使用 */
} ads1115_t;

/* ------------------------------------------------------------------ */
/* 生命周期                                                            */
/* ------------------------------------------------------------------ */

/**
 * @brief 初始化设备实例
 *
 * 内部把配置数组清零、scale 置 1.0、全部通道禁用，不做任何 I2C 探测。
 * 四路都未使能时轮询会返回 ADS1115_ERR_NO_ENABLED，这是合法的初始态。
 */
ads1115_err_t ads1115_init(ads1115_t *dev, const ads1115_port_t *port, uint8_t i2c_addr);

/* ------------------------------------------------------------------ */
/* 配置管理                                                            */
/* ------------------------------------------------------------------ */

/** 覆盖单个通道配置，并同步 enabled_mask 对应位 */
ads1115_err_t ads1115_set_channel(ads1115_t *dev, uint8_t ch_index, const ads1115_chan_cfg_t *cfg);

/** 批量覆盖配置数组，count 可小于 ADS1115_CHANNEL_COUNT（只刷前 count 路） */
ads1115_err_t ads1115_set_channels(ads1115_t *dev, const ads1115_chan_cfg_t *cfg_array, size_t count);

/** 运行时开关单路，不改变其余配置 */
ads1115_err_t ads1115_set_enabled(ads1115_t *dev, uint8_t ch_index, bool enabled);

/** 一次性设置使能位图（例如只跑 0x01 即"只用一路"） */
ads1115_err_t ads1115_set_mask(ads1115_t *dev, uint8_t mask);

/** 读取只读视图：配置 */
const ads1115_chan_cfg_t *ads1115_get_config(const ads1115_t *dev, uint8_t ch_index);

/** 读取只读视图：最近一次结果 */
const ads1115_result_t *ads1115_get_result(const ads1115_t *dev, uint8_t ch_index);

/* ------------------------------------------------------------------ */
/* 采样：非阻塞轮询                                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief 推进一次分时轮询状态机，必须周期调用（建议 1-10 ms 一次）
 *
 * 内部自动在所有【使能】通道间轮转，每次调用推进当前阶段：
 *   IDLE -> 输出本路配置字启动转换 -> WAIT
 *   WAIT -> 超时到点后读结果、写缓存、游标前进 -> IDLE
 * 常态下本次调用可能什么都不做并返回 ADS1115_OK，这是设计预期。
 */
ads1115_err_t ads1115_poll(ads1115_t *dev);

/* ------------------------------------------------------------------ */
/* 采样：阻塞式单次读取                                                */
/* ------------------------------------------------------------------ */

/**
 * @brief 阻塞式读取指定通道一次（绕过轮询调度，用于需要立即取值的场合）
 *
 * 要求 port.delay_ms 非空。会短暂接管状态机，返回后游标指向该通道，
 * 轮询继续时不会与本次采样冲突。
 */
ads1115_err_t ads1115_read_blocking(ads1115_t *dev, uint8_t ch_index, ads1115_result_t *out);

/* ------------------------------------------------------------------ */
/* 工具函数（纯计算，无 I/O）                                          */
/* ------------------------------------------------------------------ */

/** 取 PGA 档位对应的满量程电压（单位 V，正数） */
float ads1115_pga_fsr(ads1115_pga_t pga);

/** 取数据率对应的每秒采样数 */
uint32_t ads1115_dr_sps(ads1115_dr_t dr);

/** 取某配置一次单次转换所需的毫秒数（含安全余量） */
uint32_t ads1115_conv_time_ms(ads1115_dr_t dr);

/** 把通道配置拼成 16 位配置寄存器值（OS=1 启动单次转换，比较器关闭） */
uint16_t ads1115_make_config(const ads1115_chan_cfg_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* ADS1115_H */
