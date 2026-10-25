/*
 * ニッケル水素電池1本用の電池残量センサー
 *
 * 昇圧モジュール（aaa-boost-module-v2）の SNS 端子（電池電圧 × 470k / (1M + 470k)）を
 * ADC で読み、電池電圧と残量（%）を返す。ZMK の battery.c が chosen zmk,battery として読み、
 * Bluetooth の電池残量として PC に送る。
 *
 * ADC の読み方は ZMK の battery_voltage_divider.c（MIT License, The ZMK Contributors）と同じ。
 */

#define DT_DRV_COMPAT zmk_battery_nimh_voltage_divider

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
             "zmk,battery-nimh-voltage-divider は1つだけ定義すること");

/*
 * ニッケル水素電池の電圧（mV）と残量（%）の対応。間は直線で補間する。
 * 放電中の電圧は1.2V前後で長く平らになり、最後に急に下がる。
 * 昇圧モジュールは約1.0Vで昇圧を止めるので、1.0V を 0% とする。
 * 負荷や温度で電圧は変わるので、残量はあくまで目安。
 */
static const struct {
    uint16_t mv;
    uint8_t pct;
} nimh_curve[] = {
    {1350, 100}, {1300, 90}, {1250, 75}, {1220, 60}, {1200, 45},
    {1180, 30},  {1150, 20}, {1100, 10}, {1050, 5},  {1000, 0},
};

static uint8_t nimh_mv_to_pct(uint16_t mv) {
    if (mv >= nimh_curve[0].mv) {
        return nimh_curve[0].pct;
    }
    for (size_t i = 1; i < ARRAY_SIZE(nimh_curve); i++) {
        if (mv >= nimh_curve[i].mv) {
            uint16_t mv_hi = nimh_curve[i - 1].mv, mv_lo = nimh_curve[i].mv;
            uint8_t pct_hi = nimh_curve[i - 1].pct, pct_lo = nimh_curve[i].pct;
            return pct_lo + (mv - mv_lo) * (pct_hi - pct_lo) / (mv_hi - mv_lo);
        }
    }
    return 0;
}

struct nimh_config {
    uint8_t channel;
    uint32_t output_ohm;
    uint32_t full_ohm;
};

struct nimh_data {
    const struct device *adc;
    struct adc_channel_cfg acc;
    struct adc_sequence as;
    int16_t adc_raw;
    uint16_t millivolts;
    uint8_t state_of_charge;
};

static int nimh_sample_fetch(const struct device *dev, enum sensor_channel chan) {
    struct nimh_data *data = dev->data;
    const struct nimh_config *cfg = dev->config;

    if (chan != SENSOR_CHAN_GAUGE_VOLTAGE && chan != SENSOR_CHAN_GAUGE_STATE_OF_CHARGE &&
        chan != SENSOR_CHAN_ALL) {
        return -ENOTSUP;
    }

    int rc = adc_read(data->adc, &data->as);
    data->as.calibrate = false;
    if (rc != 0) {
        LOG_DBG("ADC の読み取りに失敗: %d", rc);
        return rc;
    }

    int32_t val = data->adc_raw;
    adc_raw_to_millivolts(adc_ref_internal(data->adc), data->acc.gain, data->as.resolution, &val);
    if (val < 0) {
        val = 0;
    }

    data->millivolts = val * (uint64_t)cfg->full_ohm / cfg->output_ohm;
    data->state_of_charge = nimh_mv_to_pct(data->millivolts);
    LOG_DBG("電池 %d mV => %d %%", data->millivolts, data->state_of_charge);
    return 0;
}

static int nimh_channel_get(const struct device *dev, enum sensor_channel chan,
                            struct sensor_value *val) {
    struct nimh_data *data = dev->data;

    switch (chan) {
    case SENSOR_CHAN_GAUGE_VOLTAGE:
        val->val1 = data->millivolts / 1000;
        val->val2 = (data->millivolts % 1000) * 1000U;
        return 0;
    case SENSOR_CHAN_GAUGE_STATE_OF_CHARGE:
        val->val1 = data->state_of_charge;
        val->val2 = 0;
        return 0;
    default:
        return -ENOTSUP;
    }
}

static const struct sensor_driver_api nimh_api = {
    .sample_fetch = nimh_sample_fetch,
    .channel_get = nimh_channel_get,
};

static int nimh_init(const struct device *dev) {
    struct nimh_data *data = dev->data;
    const struct nimh_config *cfg = dev->config;

    if (!device_is_ready(data->adc)) {
        LOG_ERR("ADC が使えない");
        return -ENODEV;
    }

    data->as = (struct adc_sequence){
        .channels = BIT(0),
        .buffer = &data->adc_raw,
        .buffer_size = sizeof(data->adc_raw),
        .oversampling = 4,
        .calibrate = true,
        .resolution = 12,
    };

    /*
     * 分圧抵抗が大きい（出力インピーダンス 約320kΩ）ので、取り込み時間は最長の 40µs にする。
     * nRF52840 の SAADC は 40µs なら 800kΩ まで正しく測れる。
     */
    data->acc = (struct adc_channel_cfg){
        .gain = ADC_GAIN_1_6,
        .reference = ADC_REF_INTERNAL,
        .acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, 40),
        .input_positive = SAADC_CH_PSELP_PSELP_AnalogInput0 + cfg->channel,
    };

    return adc_channel_setup(data->adc, &data->acc);
}

static struct nimh_data nimh_data = {
    .adc = DEVICE_DT_GET(DT_IO_CHANNELS_CTLR(DT_DRV_INST(0))),
};

static const struct nimh_config nimh_cfg = {
    .channel = DT_IO_CHANNELS_INPUT(DT_DRV_INST(0)),
    .output_ohm = DT_INST_PROP(0, output_ohms),
    .full_ohm = DT_INST_PROP(0, full_ohms),
};

DEVICE_DT_INST_DEFINE(0, nimh_init, NULL, &nimh_data, &nimh_cfg, POST_KERNEL,
                      CONFIG_SENSOR_INIT_PRIORITY, &nimh_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
