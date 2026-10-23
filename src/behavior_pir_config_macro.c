/*
 * 人感センサー用マクロ
 *
 * センサーが反応したときに送るキーの手順を、フラッシュの設定領域から読み込む。
 * 設定領域は設定サイト（docs/index.html）で作った .uf2 で書き込む。
 * 設定領域が空・壊れている場合は devicetree の default-keys を使う。
 *
 * status-led を指定した場合は、センサーが反応してから次に反応できるようになるまで LED を点ける。
 * センサー出力が LOW に戻ってから status-led-hold-ms 後に消す（AM312 のブロック時間の目安）。
 *
 * 設定データの形式（リトルエンディアン、80バイト）:
 *   0  magic    "PIRC"
 *   4  version  1
 *   5  count    ステップ数（1〜16）
 *   6  tap_ms   キーを押している時間
 *   8  wait_ms  キーとキーの間の待ち時間
 *   10 reserved
 *   12 steps    &kp に渡す値 × 16。HIDページが0のステップは「追加の待ち時間(ms)」
 *   76 crc      0〜75バイト目の CRC32
 */

#define DT_DRV_COMPAT zmk_behavior_pir_config_macro

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define PIR_CONFIG_MAGIC 0x43524950 /* "PIRC" */
#define PIR_CONFIG_VERSION 1
#define PIR_CONFIG_MAX_STEPS 16

#define STEP_PAGE(step) (((step) >> 16) & 0xFF)
#define STEP_IS_WAIT(step) (STEP_PAGE(step) == 0)
#define STEP_WAIT_MS(step) ((step) & 0xFFFF)

struct pir_config_data {
    uint32_t magic;
    uint8_t version;
    uint8_t count;
    uint16_t tap_ms;
    uint16_t wait_ms;
    uint16_t reserved;
    uint32_t steps[PIR_CONFIG_MAX_STEPS];
    uint32_t crc;
} __packed;

BUILD_ASSERT(sizeof(struct pir_config_data) == 80, "設定データのサイズが設定サイトと一致しない");

struct behavior_pir_config_macro_config {
    const char *key_press_dev;
    uint8_t partition_id;
    uint16_t default_tap_ms;
    uint16_t default_wait_ms;
    size_t default_count;
    const uint32_t *default_steps;
    struct gpio_dt_spec status_led; /* 指定がなければ port が NULL */
    uint32_t status_led_hold_ms;
};

struct behavior_pir_config_macro_data {
    const struct device *dev;
    struct k_work_delayable led_off_work;
};

static void led_off_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct behavior_pir_config_macro_data *data =
        CONTAINER_OF(dwork, struct behavior_pir_config_macro_data, led_off_work);
    const struct behavior_pir_config_macro_config *cfg = data->dev->config;

    gpio_pin_set_dt(&cfg->status_led, 0);
}

static int behavior_pir_config_macro_init(const struct device *dev) {
    const struct behavior_pir_config_macro_config *cfg = dev->config;
    struct behavior_pir_config_macro_data *data = dev->data;

    data->dev = dev;
    k_work_init_delayable(&data->led_off_work, led_off_work_handler);

    if (cfg->status_led.port == NULL) {
        return 0;
    }
    if (!gpio_is_ready_dt(&cfg->status_led)) {
        LOG_ERR("LED の GPIO が使えない");
        return -ENODEV;
    }
    return gpio_pin_configure_dt(&cfg->status_led, GPIO_OUTPUT_INACTIVE);
}

static bool load_config(const struct behavior_pir_config_macro_config *cfg,
                        struct pir_config_data *data) {
    const struct flash_area *fa;

    if (flash_area_open(cfg->partition_id, &fa) != 0) {
        return false;
    }
    int err = flash_area_read(fa, 0, data, sizeof(*data));
    flash_area_close(fa);
    if (err != 0) {
        return false;
    }

    if (data->magic != PIR_CONFIG_MAGIC || data->version != PIR_CONFIG_VERSION ||
        data->count == 0 || data->count > PIR_CONFIG_MAX_STEPS) {
        return false;
    }
    return crc32_ieee((const uint8_t *)data, offsetof(struct pir_config_data, crc)) == data->crc;
}

static int on_pir_config_macro_binding_pressed(struct zmk_behavior_binding *binding,
                                               struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct behavior_pir_config_macro_config *cfg = dev->config;
    struct behavior_pir_config_macro_data *dev_data = dev->data;
    struct pir_config_data data;
    const uint32_t *steps;

    if (cfg->status_led.port != NULL) {
        k_work_cancel_delayable(&dev_data->led_off_work);
        gpio_pin_set_dt(&cfg->status_led, 1);
    }
    size_t count;
    uint32_t tap_ms, wait_ms;

    if (load_config(cfg, &data)) {
        steps = data.steps;
        count = data.count;
        tap_ms = data.tap_ms;
        wait_ms = data.wait_ms;
    } else {
        LOG_INF("設定領域が空または不正なため、既定の手順を使う");
        steps = cfg->default_steps;
        count = cfg->default_count;
        tap_ms = cfg->default_tap_ms;
        wait_ms = cfg->default_wait_ms;
    }

    struct zmk_behavior_binding kp = {.behavior_dev = cfg->key_press_dev};

    for (size_t i = 0; i < count; i++) {
        if (STEP_IS_WAIT(steps[i])) {
            /* 待ち時間ステップは直前のキーの後に足すので、ここでは飛ばす */
            continue;
        }

        /* 直後に続く待ち時間ステップを、このキーを離した後の待ち時間に加える */
        uint32_t wait_after = wait_ms;
        for (size_t j = i + 1; j < count && STEP_IS_WAIT(steps[j]); j++) {
            wait_after += STEP_WAIT_MS(steps[j]);
        }

        kp.param1 = steps[i];
        zmk_behavior_queue_add(&event, kp, true, tap_ms);
        zmk_behavior_queue_add(&event, kp, false, wait_after);
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_pir_config_macro_binding_released(struct zmk_behavior_binding *binding,
                                                struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct behavior_pir_config_macro_config *cfg = dev->config;
    struct behavior_pir_config_macro_data *data = dev->data;

    /* センサー出力が LOW に戻った。ブロック時間が終わるころに LED を消す */
    if (cfg->status_led.port != NULL) {
        k_work_reschedule(&data->led_off_work, K_MSEC(cfg->status_led_hold_ms));
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_pir_config_macro_driver_api = {
    .binding_pressed = on_pir_config_macro_binding_pressed,
    .binding_released = on_pir_config_macro_binding_released,
};

#define PIR_CONFIG_MACRO_INST(n)                                                                   \
    static const uint32_t pir_config_macro_default_steps_##n[] = DT_INST_PROP(n, default_keys);   \
    static const struct behavior_pir_config_macro_config pir_config_macro_config_##n = {           \
        .key_press_dev = DEVICE_DT_NAME(DT_INST_PHANDLE(n, key_press)),                            \
        .partition_id = DT_FIXED_PARTITION_ID(DT_INST_PHANDLE(n, config_partition)),               \
        .default_tap_ms = DT_INST_PROP(n, tap_ms),                                                 \
        .default_wait_ms = DT_INST_PROP(n, wait_ms),                                               \
        .default_count = DT_INST_PROP_LEN(n, default_keys),                                        \
        .default_steps = pir_config_macro_default_steps_##n,                                       \
        .status_led = COND_CODE_1(DT_INST_NODE_HAS_PROP(n, status_led),                            \
                                  (GPIO_DT_SPEC_GET(DT_INST_PHANDLE(n, status_led), gpios)),       \
                                  ({0})),                                                          \
        .status_led_hold_ms = DT_INST_PROP(n, status_led_hold_ms),                                 \
    };                                                                                             \
    static struct behavior_pir_config_macro_data pir_config_macro_data_##n;                        \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_pir_config_macro_init, NULL, &pir_config_macro_data_##n,   \
                            &pir_config_macro_config_##n, POST_KERNEL,                             \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_pir_config_macro_driver_api);

DT_INST_FOREACH_STATUS_OKAY(PIR_CONFIG_MACRO_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
