/**
 * @file test_ads1115.c
 * @brief 核心驱动的宿主机单元测试（不依赖 ESP-IDF / 任何硬件）
 *
 * 存在意义：证明 ads1115.c 真的是「纯 C」——把 I²C 换成内存里的假寄存器，
 * 就能在 PC 上把分时轮询、逐路量程、换算链路全部跑通。
 *
 * 编译运行：
 *   cd test/host && make && ./test_ads1115
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#include "ads1115.h"

/* ================================================================== */
/* 测试脚手架                                                          */
/* ================================================================== */

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (cond) {                                         \
            g_pass++;                                       \
            printf("    [PASS] " __VA_ARGS__);               \
            printf("\n");                                   \
        } else {                                            \
            g_fail++;                                       \
            printf("    [FAIL] " __VA_ARGS__);               \
            printf("\n");                                   \
        }                                                   \
    } while (0)

#define CHECK_NEAR(a, b, tol, ...) \
    CHECK(fabsf((float)(a) - (float)(b)) <= (float)(tol), __VA_ARGS__)

/* ================================================================== */
/* 假 ADS1115：寄存器级模拟                                            */
/* ================================================================== */

typedef struct {
    uint16_t regs[4];
    uint32_t now_ms;
    uint32_t write_count;
    uint32_t read_count;
    uint8_t  last_addr;
    uint8_t  last_mux;
    bool     inject_io_error;
} mock_bus_t;

/**
 * 模拟外部真实信号：不同输入选择接不同的物理电压。
 * 差分 AIN0-AIN1 设为 0.250 V —— 正是 250 mΩ 采样电阻上流过 1 A 的压降。
 */
static float mock_signal_volts(uint8_t mux)
{
    switch (mux) {
    case ADS1115_MUX_AIN0_AIN1: return 0.250f;
    case ADS1115_MUX_AIN2_GND:  return 3.900f;
    case ADS1115_MUX_AIN3_GND:  return 1.650f;
    case ADS1115_MUX_AIN1_GND:  return 1.000f;
    default:                    return 0.0f;
    }
}

/** 按配置字里的 MUX/PGA 算出「芯片应该输出」的码值 */
static uint16_t mock_convert(uint16_t config)
{
    uint8_t mux = (uint8_t)((config >> 12) & 0x07u);
    uint8_t pga = (uint8_t)((config >> 9) & 0x07u);
    float   fsr = ads1115_pga_fsr((ads1115_pga_t)pga);
    float   volts = mock_signal_volts(mux);
    float   code = (volts / fsr) * 32768.0f;
    int32_t icode;

    if (code > 32767.0f)  { code = 32767.0f;  }
    if (code < -32768.0f) { code = -32768.0f; }

    icode = (int32_t)((code >= 0.0f) ? (code + 0.5f) : (code - 0.5f));
    return (uint16_t)(int16_t)icode;
}

static ads1115_err_t mock_transfer(void *ctx, uint8_t dev_addr,
                                   const uint8_t *tx, size_t tx_len,
                                   uint8_t *rx, size_t rx_len)
{
    mock_bus_t *bus = (mock_bus_t *)ctx;
    uint8_t reg;

    if (bus == NULL || tx == NULL || tx_len < 1u) {
        return ADS1115_ERR_IO;
    }
    if (bus->inject_io_error) {
        return ADS1115_ERR_IO;
    }

    bus->last_addr = dev_addr;
    reg = tx[0];
    if (reg > ADS1115_REG_HI_THRESH) {
        return ADS1115_ERR_IO;
    }

    if ((rx != NULL) && (rx_len > 0u)) {
        if ((tx_len != 1u) || (rx_len != 2u)) {
            return ADS1115_ERR_IO;
        }
        bus->read_count++;
        if (reg == ADS1115_REG_CONVERSION) {
            bus->last_mux = (uint8_t)((bus->regs[ADS1115_REG_CONFIG] >> 12) & 0x07u);
            bus->regs[ADS1115_REG_CONVERSION] = mock_convert(bus->regs[ADS1115_REG_CONFIG]);
        }
        rx[0] = (uint8_t)(bus->regs[reg] >> 8);
        rx[1] = (uint8_t)(bus->regs[reg] & 0xFFu);
    } else {
        if (tx_len != 3u) {
            return ADS1115_ERR_IO;
        }
        bus->write_count++;
        bus->regs[reg] = (uint16_t)(((uint16_t)tx[1] << 8) | (uint16_t)tx[2]);
    }

    return ADS1115_OK;
}

/* 虚拟时钟：让 poll 的等待逻辑在测试中瞬时完成 */
static void     mock_delay_ms(void *ctx, uint32_t ms) { ((mock_bus_t *)ctx)->now_ms += ms; }
static uint32_t mock_now_ms(void *ctx)                { return ((mock_bus_t *)ctx)->now_ms; }

static void mock_reset(mock_bus_t *bus)
{
    memset(bus, 0, sizeof(*bus));
}

static void attach_mock(ads1115_t *dev, ads1115_port_t *port, mock_bus_t *bus)
{
    port->transfer = mock_transfer;
    port->delay_ms = mock_delay_ms;
    port->now_ms   = mock_now_ms;
    port->ctx      = bus;

    (void)ads1115_init(dev, port, ADS1115_I2C_ADDR_MIN);
}

/** 推进 n 次 poll，每次把虚拟时钟前推 step_ms */
static void run_polls(ads1115_t *dev, mock_bus_t *bus, int n, uint32_t step_ms)
{
    int i;

    for (i = 0; i < n; i++) {
        bus->now_ms += step_ms;
        (void)ads1115_poll(dev);
    }
}

static ads1115_chan_cfg_t make_cfg(ads1115_mux_t mux, ads1115_pga_t pga, ads1115_dr_t dr,
                                   bool enabled, float scale, float offset)
{
    ads1115_chan_cfg_t c;

    c.mux     = mux;
    c.pga     = pga;
    c.dr      = dr;
    c.enabled = enabled;
    c.scale   = scale;
    c.offset  = offset;

    return c;
}

/* ================================================================== */
/* 用例 1：配置字拼装                                                  */
/* ================================================================== */

static void test_make_config(void)
{
    ads1115_chan_cfg_t c;
    uint16_t w;

    printf("\n[1] 配置字拼装 (OS=1 / single-shot / 比较器关闭)\n");

    c = make_cfg(ADS1115_MUX_AIN0_AIN1, ADS1115_PGA_0V512, ADS1115_DR_128SPS, true, 1.0f, 0.0f);
    w = ads1115_make_config(&c);
    CHECK(w == 0x8983u, "AIN0-AIN1 差分 +-0.512V + 128SPS -> 0x%04X (期望 0x8983)", w);

    c = make_cfg(ADS1115_MUX_AIN0_GND, ADS1115_PGA_4V096, ADS1115_DR_128SPS, true, 1.0f, 0.0f);
    w = ads1115_make_config(&c);
    CHECK(w == 0xC383u, "AIN0 单端 +-4.096V + 128SPS -> 0x%04X (期望 0xC383)", w);

    c = make_cfg(ADS1115_MUX_AIN3_GND, ADS1115_PGA_0V256, ADS1115_DR_860SPS, true, 1.0f, 0.0f);
    w = ads1115_make_config(&c);
    CHECK(w == 0xFBE3u, "AIN3 单端 +-0.256V + 860SPS -> 0x%04X (期望 0xFBE3)", w);

    c = make_cfg(ADS1115_MUX_AIN0_AIN1, ADS1115_PGA_0V256, ADS1115_DR_860SPS, true, 1.0f, 0.0f);
    w = ads1115_make_config(&c);
    CHECK(w == 0x8BE3u, "AIN0-AIN1 差分 +-0.256V + 860SPS -> 0x%04X (期望 0x8BE3)", w);

    CHECK(ads1115_make_config(NULL) == 0u, "空指针返回 0");
}

/* ================================================================== */
/* 用例 2：四路全开，逐路独立量程                                      */
/* ================================================================== */

static void test_quad_enabled(void)
{
    static ads1115_t dev;
    static ads1115_port_t port;
    mock_bus_t bus;
    const ads1115_result_t *r;

    printf("\n[2] 四路全开，每路独立量程与换算\n");

    mock_reset(&bus);
    attach_mock(&dev, &port, &bus);

    ads1115_chan_cfg_t cfgs[ADS1115_CHANNEL_COUNT] = {
        /* CH0: 差分 250mΩ 电流采样，尺度换算到安培 */
        make_cfg(ADS1115_MUX_AIN0_AIN1, ADS1115_PGA_0V512, ADS1115_DR_128SPS, true, 1.0f / 0.25f, 0.0f),
        /* CH1: 单端电池分压，尺度换算乘 2 */
        make_cfg(ADS1115_MUX_AIN2_GND,  ADS1115_PGA_6V144, ADS1115_DR_8SPS,   true, 2.0f, 0.0f),
        /* CH2: 单端 NTC，原始电压 */
        make_cfg(ADS1115_MUX_AIN3_GND,  ADS1115_PGA_4V096, ADS1115_DR_16SPS,  true, 1.0f, 0.0f),
        /* CH3: 禁用 */
        make_cfg(ADS1115_MUX_AIN1_GND,  ADS1115_PGA_2V048, ADS1115_DR_32SPS,  false, 1.0f, 0.0f),
    };

    CHECK(ads1115_set_channels(&dev, cfgs, ADS1115_CHANNEL_COUNT) == ADS1115_OK, "批量下发四路配置");
    CHECK(dev.enabled_mask == 0x07u, "使能位图 = 0x%02X (期望 0x07，CH3 关闭)", dev.enabled_mask);

    /* 8 次 poll 足够轮完三路（每路两拍） */
    run_polls(&dev, &bus, 8, 200u);

    r = ads1115_get_result(&dev, 0);
    CHECK(r->valid, "CH0 已产生有效结果");
    CHECK_NEAR(r->volt, 0.250f, 0.001f, "CH0 电压 = %.4f V (期望 0.250)", r->volt);
    CHECK_NEAR(r->value, 1.0f, 0.01f, "CH0 工程值 = %.4f A (期望 1.000)", r->value);

    r = ads1115_get_result(&dev, 1);
    CHECK(r->valid, "CH1 已产生有效结果");
    CHECK_NEAR(r->volt, 3.900f, 0.01f, "CH1 电压 = %.4f V (期望 3.900)", r->volt);
    CHECK_NEAR(r->value, 7.800f, 0.02f, "CH1 工程值 = %.4f V (分压还原，期望 7.800)", r->value);

    r = ads1115_get_result(&dev, 2);
    CHECK(r->valid, "CH2 已产生有效结果");
    CHECK_NEAR(r->volt, 1.650f, 0.01f, "CH2 电压 = %.4f V (期望 1.650)", r->volt);

    r = ads1115_get_result(&dev, 3);
    CHECK(!r->valid, "CH3 被禁用，未产生结果");

    printf("    --- 轮询统计：写入 %lu 次配置，读取 %lu 次转换寄存器 ---\n",
           (unsigned long)bus.write_count, (unsigned long)bus.read_count);
}

/* ================================================================== */
/* 用例 3：只启用一路                                                  */
/* ================================================================== */

static void test_single_enabled(void)
{
    static ads1115_t dev;
    static ads1115_port_t port;
    mock_bus_t bus;
    uint32_t seq_ch1_before;
    uint32_t seq_ch2_before;

    printf("\n[3] 只用一路：set_mask 动态收敛\n");

    mock_reset(&bus);
    attach_mock(&dev, &port, &bus);

    ads1115_chan_cfg_t cfgs[ADS1115_CHANNEL_COUNT] = {
        make_cfg(ADS1115_MUX_AIN0_AIN1, ADS1115_PGA_0V512, ADS1115_DR_128SPS, true, 1.0f / 0.25f, 0.0f),
        make_cfg(ADS1115_MUX_AIN2_GND,  ADS1115_PGA_6V144, ADS1115_DR_8SPS,   true, 2.0f, 0.0f),
        make_cfg(ADS1115_MUX_AIN3_GND,  ADS1115_PGA_4V096, ADS1115_DR_16SPS,  true, 1.0f, 0.0f),
        make_cfg(ADS1115_MUX_AIN1_GND,  ADS1115_PGA_2V048, ADS1115_DR_32SPS,  false, 1.0f, 0.0f),
    };
    (void)ads1115_set_channels(&dev, cfgs, ADS1115_CHANNEL_COUNT);

    run_polls(&dev, &bus, 8, 200u);

    seq_ch1_before = ads1115_get_result(&dev, 1)->seq;
    seq_ch2_before = ads1115_get_result(&dev, 2)->seq;
    CHECK(seq_ch1_before > 0u, "切换前 CH1 有结果 seq=%lu", (unsigned long)seq_ch1_before);

    /* 收敛到只有 CH0 */
    CHECK(ads1115_set_mask(&dev, (uint8_t)(1u << 0)) == ADS1115_OK, "set_mask(0x01) 成功");
    CHECK(dev.enabled_mask == 0x01u, "使能位图 = 0x%02X", dev.enabled_mask);
    CHECK(dev.ch[1].enabled == false, "CH1 的 enabled 字段同步为 false");

    run_polls(&dev, &bus, 8, 200u);

    CHECK(ads1115_get_result(&dev, 1)->seq == seq_ch1_before, "CH1 不再被刷新");
    CHECK(ads1115_get_result(&dev, 2)->seq == seq_ch2_before, "CH2 不再被刷新");
    CHECK(ads1115_get_result(&dev, 0)->seq > 0u, "CH0 仍在持续刷新 seq=%lu",
          (unsigned long)ads1115_get_result(&dev, 0)->seq);

    /* 恢复四路（CH3 未使能，实际三路） */
    CHECK(ads1115_set_mask(&dev, 0x0Fu) == ADS1115_OK, "set_mask(0x0F) 恢复");
    CHECK(dev.enabled_mask == 0x0Fu, "使能位图恢复为 0x0F");

    /* 单路开关 */
    CHECK(ads1115_set_enabled(&dev, 2u, false) == ADS1115_OK, "set_enabled(CH2, false)");
    CHECK((dev.enabled_mask & (1u << 2)) == 0u, "CH2 位被清除");
}

/* ================================================================== */
/* 用例 4：同一物理量在不同量程下还原一致                              */
/* ================================================================== */

static void test_pga_independence(void)
{
    static ads1115_t dev;
    static ads1115_port_t port;
    mock_bus_t bus;
    int16_t raw_wide;
    int16_t raw_fine;

    printf("\n[4] 逐路量程独立：同一电压在两档 PGA 下都还原正确\n");

    mock_reset(&bus);
    attach_mock(&dev, &port, &bus);

    ads1115_chan_cfg_t cfgs[ADS1115_CHANNEL_COUNT] = {
        make_cfg(ADS1115_MUX_AIN0_AIN1, ADS1115_PGA_6V144, ADS1115_DR_128SPS, true, 1.0f, 0.0f),
        make_cfg(ADS1115_MUX_AIN0_AIN1, ADS1115_PGA_0V512, ADS1115_DR_128SPS, true, 1.0f, 0.0f),
        make_cfg(ADS1115_MUX_AIN0_AIN1, ADS1115_PGA_0V256, ADS1115_DR_128SPS, true, 1.0f, 0.0f),
        make_cfg(ADS1115_MUX_AIN0_AIN1, ADS1115_PGA_2V048, ADS1115_DR_128SPS, false, 1.0f, 0.0f),
    };
    (void)ads1115_set_channels(&dev, cfgs, ADS1115_CHANNEL_COUNT);

    run_polls(&dev, &bus, 8, 200u);

    raw_wide = ads1115_get_result(&dev, 0)->raw;
    raw_fine = ads1115_get_result(&dev, 1)->raw;

    CHECK_NEAR(ads1115_get_result(&dev, 0)->volt, 0.250f, 0.001f, "+-6.144V 档还原 %.4f V",
               ads1115_get_result(&dev, 0)->volt);
    CHECK_NEAR(ads1115_get_result(&dev, 1)->volt, 0.250f, 0.001f, "+-0.512V 档还原 %.4f V",
               ads1115_get_result(&dev, 1)->volt);
    CHECK_NEAR(ads1115_get_result(&dev, 2)->volt, 0.250f, 0.001f, "+-0.256V 档还原 %.4f V",
               ads1115_get_result(&dev, 2)->volt);

    CHECK(fabsf((float)raw_fine - ((float)raw_wide * 12.0f)) <= 16.0f,
          "码值比例符合量程比：%d vs %d (约 12 倍)", (int)raw_fine, (int)raw_wide);

    CHECK_NEAR(ads1115_pga_fsr(ADS1115_PGA_0V512), 0.512f, 1e-6f, "pga_fsr(0V512) = 0.512");
    CHECK(ads1115_dr_sps(ADS1115_DR_860SPS) == 860u, "dr_sps(860SPS) = 860");
    CHECK(ads1115_conv_time_ms(ADS1115_DR_860SPS) == 4u, "860SPS 转换耗时 %u ms",
          (unsigned)ads1115_conv_time_ms(ADS1115_DR_860SPS));
    CHECK(ads1115_conv_time_ms(ADS1115_DR_8SPS) == 127u, "8SPS 转换耗时 %u ms",
          (unsigned)ads1115_conv_time_ms(ADS1115_DR_8SPS));
}

/* ================================================================== */
/* 用例 5：参数校验与边界                                              */
/* ================================================================== */

static void test_param_validation(void)
{
    static ads1115_t dev;
    static ads1115_port_t port;
    mock_bus_t bus;
    ads1115_chan_cfg_t good;

    printf("\n[5] 参数校验与边界\n");

    mock_reset(&bus);
    attach_mock(&dev, &port, &bus);

    good = make_cfg(ADS1115_MUX_AIN0_GND, ADS1115_PGA_2V048, ADS1115_DR_128SPS, true, 1.0f, 0.0f);

    CHECK(ads1115_init(NULL, &port, 0x48u) == ADS1115_ERR_PARAM, "init(NULL) 拒绝");
    CHECK(ads1115_init(&dev, NULL, 0x48u) == ADS1115_ERR_PARAM, "init(port=NULL) 拒绝");
    CHECK(ads1115_init(&dev, &port, 0x50u) == ADS1115_ERR_PARAM, "地址 0x50 越界拒绝");
    CHECK(ads1115_init(&dev, &port, 0x48u) == ADS1115_OK, "地址 0x48 合法");

    CHECK(ads1115_set_channel(&dev, 4u, &good) == ADS1115_ERR_PARAM, "通道 4 越界拒绝");
    CHECK(ads1115_set_channel(&dev, 0u, NULL) == ADS1115_ERR_PARAM, "cfg=NULL 拒绝");

    good.pga = (ads1115_pga_t)0x07;
    CHECK(ads1115_set_channel(&dev, 0u, &good) == ADS1115_ERR_PARAM, "PGA=0x07 非法拒绝");

    good.pga = ADS1115_PGA_2V048;
    good.dr = (ads1115_dr_t)0x08;
    CHECK(ads1115_set_channel(&dev, 0u, &good) == ADS1115_ERR_PARAM, "DR=0x08 非法拒绝");

    good.dr = ADS1115_DR_128SPS;
    good.mux = (ads1115_mux_t)0x08;
    CHECK(ads1115_set_channel(&dev, 0u, &good) == ADS1115_ERR_PARAM, "MUX=0x08 非法拒绝");

    /* 全禁用时 poll 返回 NO_ENABLED，且不是致命的 */
    CHECK(dev.enabled_mask == 0u, "初始全禁用");
    CHECK(ads1115_poll(&dev) == ADS1115_ERR_NO_ENABLED, "全禁用时 poll 返回 NO_ENABLED");

    CHECK(ads1115_get_config(&dev, 9u) == NULL, "get_config 越界返回 NULL");
    CHECK(ads1115_get_result(&dev, 9u) == NULL, "get_result 越界返回 NULL");

    good.mux = ADS1115_MUX_AIN0_GND;
    CHECK(ads1115_set_channel(&dev, 0u, &good) == ADS1115_OK, "恢复合法配置");
    CHECK(ads1115_set_channels(&dev, NULL, 4u) == ADS1115_ERR_PARAM, "set_channels(NULL) 拒绝");
    CHECK(ads1115_set_channels(&dev, &good, 9u) == ADS1115_ERR_PARAM, "count 越界拒绝");
}

/* ================================================================== */
/* 用例 6：I/O 故障后状态机可恢复                                      */
/* ================================================================== */

static void test_io_recovery(void)
{
    static ads1115_t dev;
    static ads1115_port_t port;
    mock_bus_t bus;
    ads1115_chan_cfg_t cfg;

    printf("\n[6] I2C 故障后状态机不锁死\n");

    mock_reset(&bus);
    attach_mock(&dev, &port, &bus);

    cfg = make_cfg(ADS1115_MUX_AIN2_GND, ADS1115_PGA_6V144, ADS1115_DR_128SPS, true, 1.0f, 0.0f);
    (void)ads1115_set_channel(&dev, 0u, &cfg);

    bus.inject_io_error = true;
    CHECK(ads1115_poll(&dev) == ADS1115_ERR_IO, "故障时 poll 返回 ERR_IO");
    CHECK(dev.state == ADS1115_STATE_IDLE, "故障后状态机回到 IDLE");

    bus.inject_io_error = false;
    run_polls(&dev, &bus, 4, 200u);
    CHECK(ads1115_get_result(&dev, 0)->valid, "故障解除后恢复正常采样，volt=%.4f V",
          ads1115_get_result(&dev, 0)->volt);
}

/* ================================================================== */
/* 用例 7：阻塞式单次读取 + 中途改量程                                  */
/* ================================================================== */

static void test_blocking_and_reconfig(void)
{
    static ads1115_t dev;
    static ads1115_port_t port;
    mock_bus_t bus;
    ads1115_result_t out;
    ads1115_chan_cfg_t cfg;

    printf("\n[7] 阻塞读取与运行期改量程\n");

    mock_reset(&bus);
    attach_mock(&dev, &port, &bus);

    cfg = make_cfg(ADS1115_MUX_AIN0_AIN1, ADS1115_PGA_6V144, ADS1115_DR_128SPS, true, 1.0f / 0.25f, 0.0f);
    (void)ads1115_set_channel(&dev, 0u, &cfg);

    CHECK(ads1115_read_blocking(&dev, 0u, &out) == ADS1115_OK, "阻塞读取成功");
    CHECK_NEAR(out.volt, 0.250f, 0.001f, "阻塞读取 volt = %.4f V", out.volt);
    CHECK_NEAR(out.value, 1.0f, 0.01f, "阻塞读取 value = %.4f A", out.value);

    /* 运行期把量程从宽档切到细档，读数应保持一致 */
    cfg.pga = ADS1115_PGA_0V512;
    CHECK(ads1115_set_channel(&dev, 0u, &cfg) == ADS1115_OK, "运行期改 PGA 为 +-0.512V");

    CHECK(ads1115_read_blocking(&dev, 0u, &out) == ADS1115_OK, "改量程后再读成功");
    CHECK_NEAR(out.volt, 0.250f, 0.001f, "改量程后 volt 仍为 %.4f V", out.volt);
    CHECK(out.raw >= 15999 && out.raw <= 16001, "细档码值 = %d (期望约 16000)", (int)out.raw);

    CHECK(ads1115_read_blocking(&dev, 9u, &out) == ADS1115_ERR_PARAM, "通道越界拒绝");
    CHECK(ads1115_read_blocking(&dev, 0u, NULL) == ADS1115_OK, "out 允许为 NULL");
}

/* ================================================================== */

int main(void)
{
    printf("=========== ADS1115 核心驱动宿主机测试 ===========\n");

    test_make_config();
    test_quad_enabled();
    test_single_enabled();
    test_pga_independence();
    test_param_validation();
    test_io_recovery();
    test_blocking_and_reconfig();

    printf("\n=========== 结果：%d 通过 / %d 失败 ===========\n", g_pass, g_fail);

    return (g_fail == 0) ? 0 : 1;
}
