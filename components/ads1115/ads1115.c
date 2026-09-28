/**
 * @file ads1115.c
 * @brief ADS1115 驱动核心实现 —— 零平台依赖
 *
 * 本文件不得 include 任何 ESP-IDF / FreeRTOS / HAL 头文件。
 * 所有硬件访问均经由 dev->port 中的函数指针完成。
 */

#include "ads1115.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* 静态查表                                                           */
/* ------------------------------------------------------------------ */

/** 满量程电压，下标 = ads1115_pga_t */
static const float s_fsr[6] = {
    6.144f, 4.096f, 2.048f, 1.024f, 0.512f, 0.256f,
};

/** 每秒采样数，下标 = ads1115_dr_t */
static const uint32_t s_sps[8] = {
    8u, 16u, 32u, 64u, 128u, 250u, 475u, 860u,
};

/** 结果码换算上限（有符号 16 位正半轴） */
#define ADS1115_FULL_SCALE_CODE 32768.0f

/* ------------------------------------------------------------------ */
/* 内部辅助                                                           */
/* ------------------------------------------------------------------ */

static bool pga_is_valid(ads1115_pga_t pga)
{
    return ((int)pga >= (int)ADS1115_PGA_6V144) && ((int)pga <= (int)ADS1115_PGA_0V256);
}

static bool dr_is_valid(ads1115_dr_t dr)
{
    return ((int)dr >= (int)ADS1115_DR_8SPS) && ((int)dr <= (int)ADS1115_DR_860SPS);
}

static bool mux_is_valid(ads1115_mux_t mux)
{
    return ((int)mux >= (int)ADS1115_MUX_AIN0_AIN1) && ((int)mux <= (int)ADS1115_MUX_AIN3_GND);
}

static bool dev_is_ready(const ads1115_t *dev)
{
    return (dev != NULL) && dev->inited && (dev->port.transfer != NULL);
}

static bool addr_is_valid(uint8_t addr)
{
    return (addr >= ADS1115_I2C_ADDR_MIN) && (addr <= ADS1115_I2C_ADDR_MAX);
}

/** 从 from 开始（含）环形查找下一个使能通道，找不到返回 0xFF */
static uint8_t next_enabled_from(const ads1115_t *dev, uint8_t from)
{
    uint8_t i;

    for (i = 0u; i < ADS1115_CHANNEL_COUNT; i++) {
        uint8_t idx = (uint8_t)((from + i) % ADS1115_CHANNEL_COUNT);

        if ((dev->enabled_mask & (uint8_t)(1u << idx)) != 0u) {
            return idx;
        }
    }
    return 0xFFu;
}

/** 取毫秒时间戳，port 未提供则返回 0 */
static uint32_t port_now_ms(ads1115_t *dev)
{
    if (dev->port.now_ms == NULL) {
        return 0u;
    }
    return dev->port.now_ms(dev->port.ctx);
}

/** 判断本次转换的等待时间是否已经到点 */
static bool wait_is_satisfied(ads1115_t *dev)
{
    if (dev->port.now_ms != NULL) {
        uint32_t now = dev->port.now_ms(dev->port.ctx);

        return (int32_t)(now - dev->wait_until) >= 0;
    }

    if (dev->port.delay_ms != NULL) {
        dev->port.delay_ms(dev->port.ctx, dev->wait_span != 0u ? dev->wait_span : 1u);
        return true;
    }

    /* 两个回调都没有：假定调用方给的 poll 周期已经足够长 */
    return true;
}

/** 写 16 位寄存器 */
static ads1115_err_t write_register(ads1115_t *dev, uint8_t reg, uint16_t value)
{
    uint8_t tx[3];

    tx[0] = reg;
    tx[1] = (uint8_t)(value >> 8);
    tx[2] = (uint8_t)(value & 0xFFu);

    return dev->port.transfer(dev->port.ctx, dev->i2c_addr, tx, sizeof(tx), NULL, 0u);
}

/** 读 16 位寄存器 */
static ads1115_err_t read_register(ads1115_t *dev, uint8_t reg, uint16_t *value)
{
    uint8_t tx[1];
    uint8_t rx[2];
    ads1115_err_t err;

    tx[0] = reg;

    err = dev->port.transfer(dev->port.ctx, dev->i2c_addr, tx, sizeof(tx), rx, sizeof(rx));
    if (err != ADS1115_OK) {
        return err;
    }

    *value = (uint16_t)(((uint16_t)rx[0] << 8) | (uint16_t)rx[1]);
    return ADS1115_OK;
}

/** 把 16 位原始码值按通道配置填进结果缓存 */
static void store_result(ads1115_t *dev, uint8_t ch_index, uint16_t raw_u16)
{
    const ads1115_chan_cfg_t *cfg = &dev->ch[ch_index];
    ads1115_result_t         *res = &dev->result[ch_index];
    int16_t raw = (int16_t)raw_u16;
    float   volt;

    volt = ((float)raw * ads1115_pga_fsr(cfg->pga)) / ADS1115_FULL_SCALE_CODE;

    res->raw   = raw;
    res->volt  = volt;
    res->value = (volt * cfg->scale) + cfg->offset;
    res->seq   = ++dev->seq;
    res->valid = true;
}

/* ------------------------------------------------------------------ */
/* 工具函数                                                           */
/* ------------------------------------------------------------------ */

float ads1115_pga_fsr(ads1115_pga_t pga)
{
    if (!pga_is_valid(pga)) {
        return s_fsr[(int)ADS1115_PGA_2V048];
    }
    return s_fsr[(int)pga];
}

uint32_t ads1115_dr_sps(ads1115_dr_t dr)
{
    if (!dr_is_valid(dr)) {
        return s_sps[(int)ADS1115_DR_128SPS];
    }
    return s_sps[(int)dr];
}

uint32_t ads1115_conv_time_ms(ads1115_dr_t dr)
{
    uint32_t sps = ads1115_dr_sps(dr);

    /* 向上取整到毫秒，再加固定余量覆盖 I2C 往返与内部建立 */
    return ((1000u + sps - 1u) / sps) + ADS1115_CONV_MARGIN_MS;
}

uint16_t ads1115_make_config(const ads1115_chan_cfg_t *cfg)
{
    uint16_t reg = 0u;

    if (cfg == NULL) {
        return 0u;
    }

    /* bit15 OS=1：单次转换模式下写 1 即启动一次转换 */
    reg |= (uint16_t)0x8000u;
    /* bit14:12 MUX[2:0] */
    reg |= (uint16_t)(((uint16_t)cfg->mux & 0x07u) << 12);
    /* bit11:9  PGA[2:0] */
    reg |= (uint16_t)(((uint16_t)cfg->pga & 0x07u) << 9);
    /* bit8     MODE=1：单次转换（分时复用的前提） */
    reg |= (uint16_t)(1u << 8);
    /* bit7:5   DR[2:0] */
    reg |= (uint16_t)(((uint16_t)cfg->dr & 0x07u) << 5);
    /* bit4:2   比较器配置保持 0（不启用） */
    /* bit1:0   COMP_QUE=11：关闭比较器、ALERT/RDY 置为高阻 */
    reg |= (uint16_t)0x0003u;

    return reg;
}

/* ------------------------------------------------------------------ */
/* 生命周期                                                           */
/* ------------------------------------------------------------------ */

ads1115_err_t ads1115_init(ads1115_t *dev, const ads1115_port_t *port, uint8_t i2c_addr)
{
    size_t i;

    if ((dev == NULL) || (port == NULL) || (port->transfer == NULL)) {
        return ADS1115_ERR_PARAM;
    }
    if (!addr_is_valid(i2c_addr)) {
        return ADS1115_ERR_PARAM;
    }

    memset(dev, 0, sizeof(*dev));

    dev->port     = *port;
    dev->i2c_addr = i2c_addr;
    dev->inited   = true;
    dev->cursor   = (uint8_t)(ADS1115_CHANNEL_COUNT - 1u); /* 轮转从通道 0 开始 */
    dev->state    = ADS1115_STATE_IDLE;

    for (i = 0u; i < ADS1115_CHANNEL_COUNT; i++) {
        dev->ch[i].mux      = ADS1115_MUX_AIN0_GND;
        dev->ch[i].pga      = ADS1115_PGA_2V048;
        dev->ch[i].dr       = ADS1115_DR_128SPS;
        dev->ch[i].enabled  = false;
        dev->ch[i].scale    = 1.0f;
        dev->ch[i].offset   = 0.0f;
    }

    return ADS1115_OK;
}

/* ------------------------------------------------------------------ */
/* 配置管理                                                           */
/* ------------------------------------------------------------------ */

ads1115_err_t ads1115_set_channel(ads1115_t *dev, uint8_t ch_index, const ads1115_chan_cfg_t *cfg)
{
    if (!dev_is_ready(dev) || (cfg == NULL) || (ch_index >= ADS1115_CHANNEL_COUNT)) {
        return ADS1115_ERR_PARAM;
    }
    if (!mux_is_valid(cfg->mux) || !pga_is_valid(cfg->pga) || !dr_is_valid(cfg->dr)) {
        return ADS1115_ERR_PARAM;
    }

    dev->ch[ch_index] = *cfg;

    if (cfg->enabled) {
        dev->enabled_mask |= (uint8_t)(1u << ch_index);
    } else {
        dev->enabled_mask &= (uint8_t)~(1u << ch_index);
    }

    return ADS1115_OK;
}

ads1115_err_t ads1115_set_channels(ads1115_t *dev, const ads1115_chan_cfg_t *cfg_array, size_t count)
{
    size_t i;
    ads1115_err_t err;

    if (!dev_is_ready(dev) || (cfg_array == NULL) || (count == 0u)) {
        return ADS1115_ERR_PARAM;
    }
    if (count > ADS1115_CHANNEL_COUNT) {
        return ADS1115_ERR_PARAM;
    }

    for (i = 0u; i < count; i++) {
        err = ads1115_set_channel(dev, (uint8_t)i, &cfg_array[i]);
        if (err != ADS1115_OK) {
            return err;
        }
    }

    return ADS1115_OK;
}

ads1115_err_t ads1115_set_enabled(ads1115_t *dev, uint8_t ch_index, bool enabled)
{
    if (!dev_is_ready(dev) || (ch_index >= ADS1115_CHANNEL_COUNT)) {
        return ADS1115_ERR_PARAM;
    }

    dev->ch[ch_index].enabled = enabled;

    if (enabled) {
        dev->enabled_mask |= (uint8_t)(1u << ch_index);
    } else {
        dev->enabled_mask &= (uint8_t)~(1u << ch_index);

        /* 被禁用的正是当前在采/待采通道时，立即让游标归一 */
        if (dev->active == ch_index) {
            dev->state = ADS1115_STATE_IDLE;
        }
    }

    return ADS1115_OK;
}

ads1115_err_t ads1115_set_mask(ads1115_t *dev, uint8_t mask)
{
    uint8_t i;

    if (!dev_is_ready(dev)) {
        return ADS1115_ERR_PARAM;
    }

    mask &= (uint8_t)((1u << ADS1115_CHANNEL_COUNT) - 1u);

    dev->enabled_mask = mask;

    for (i = 0u; i < ADS1115_CHANNEL_COUNT; i++) {
        dev->ch[i].enabled = ((mask & (uint8_t)(1u << i)) != 0u);
    }

    if ((mask & (uint8_t)(1u << dev->active)) == 0u) {
        dev->state = ADS1115_STATE_IDLE;
    }

    return ADS1115_OK;
}

const ads1115_chan_cfg_t *ads1115_get_config(const ads1115_t *dev, uint8_t ch_index)
{
    if ((dev == NULL) || (ch_index >= ADS1115_CHANNEL_COUNT)) {
        return NULL;
    }
    return &dev->ch[ch_index];
}

const ads1115_result_t *ads1115_get_result(const ads1115_t *dev, uint8_t ch_index)
{
    if ((dev == NULL) || (ch_index >= ADS1115_CHANNEL_COUNT)) {
        return NULL;
    }
    return &dev->result[ch_index];
}

/* ------------------------------------------------------------------ */
/* 非阻塞轮询                                                          */
/* ------------------------------------------------------------------ */

ads1115_err_t ads1115_poll(ads1115_t *dev)
{
    if (!dev_is_ready(dev)) {
        return ADS1115_ERR_PARAM;
    }
    if (dev->enabled_mask == 0u) {
        return ADS1115_ERR_NO_ENABLED;
    }

    switch (dev->state) {
    case ADS1115_STATE_IDLE: {
        uint8_t  ch;
        uint16_t config_word;
        ads1115_err_t err;

        ch = next_enabled_from(dev, (uint8_t)(dev->cursor + 1u));
        if (ch == 0xFFu) {
            return ADS1115_ERR_NO_ENABLED;
        }

        dev->active     = ch;
        dev->wait_span  = ads1115_conv_time_ms(dev->ch[ch].dr);
        dev->wait_until = port_now_ms(dev) + dev->wait_span;

        config_word = ads1115_make_config(&dev->ch[ch]);
        err = write_register(dev, ADS1115_REG_CONFIG, config_word);
        if (err != ADS1115_OK) {
            /* 通信失败不锁死状态机，下一轮重新从同一路发起 */
            dev->state = ADS1115_STATE_IDLE;
            return err;
        }

        dev->state = ADS1115_STATE_WAIT;
        return ADS1115_OK;
    }

    case ADS1115_STATE_WAIT: {
        uint16_t raw_u16;
        ads1115_err_t err;

        if (!wait_is_satisfied(dev)) {
            return ADS1115_OK; /* 还没到点，下次 poll 继续 */
        }

        err = read_register(dev, ADS1115_REG_CONVERSION, &raw_u16);
        if (err != ADS1115_OK) {
            dev->state = ADS1115_STATE_IDLE;
            return err;
        }

        store_result(dev, dev->active, raw_u16);

        dev->cursor = dev->active;
        dev->state  = ADS1115_STATE_IDLE;
        return ADS1115_OK;
    }

    default:
        dev->state = ADS1115_STATE_IDLE;
        return ADS1115_ERR_STATE;
    }
}

/* ------------------------------------------------------------------ */
/* 阻塞式单次读取                                                      */
/* ------------------------------------------------------------------ */

ads1115_err_t ads1115_read_blocking(ads1115_t *dev, uint8_t ch_index, ads1115_result_t *out)
{
    uint16_t config_word;
    uint16_t raw_u16;
    uint32_t span_ms;
    ads1115_err_t err;

    if (!dev_is_ready(dev) || (ch_index >= ADS1115_CHANNEL_COUNT)) {
        return ADS1115_ERR_PARAM;
    }
    if (dev->port.delay_ms == NULL) {
        return ADS1115_ERR_PARAM; /* 阻塞路径必须有延时能力 */
    }
    if (!mux_is_valid(dev->ch[ch_index].mux) ||
        !pga_is_valid(dev->ch[ch_index].pga) ||
        !dr_is_valid(dev->ch[ch_index].dr)) {
        return ADS1115_ERR_PARAM;
    }

    /* 暂存轮询状态，采样完毕后做一次归一，避免游标错位 */
    span_ms     = ads1115_conv_time_ms(dev->ch[ch_index].dr);
    config_word = ads1115_make_config(&dev->ch[ch_index]);

    err = write_register(dev, ADS1115_REG_CONFIG, config_word);
    if (err != ADS1115_OK) {
        return err;
    }

    dev->port.delay_ms(dev->port.ctx, span_ms);

    err = read_register(dev, ADS1115_REG_CONVERSION, &raw_u16);
    if (err != ADS1115_OK) {
        return err;
    }

    store_result(dev, ch_index, raw_u16);

    dev->cursor = ch_index;
    dev->active = ch_index;
    dev->state  = ADS1115_STATE_IDLE;

    if (out != NULL) {
        *out = dev->result[ch_index];
    }

    return ADS1115_OK;
}
