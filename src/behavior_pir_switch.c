/*
 * PIR Switch 本体の動作
 *
 * &pir <コマンド> でキーマップから呼ぶ（コマンドは dt-bindings/zmk/pir_switch.h）。
 *   PIR_TRIGGER    センサー: 今のモードのキーを送り、次に反応できるまで LED を点ける
 *   PIR_TEST       動作確認: 今のモードのキーを送り、モードの色で1回点滅
 *   PIR_MODE_1/2   モードを切り替え、新しいモードの色で3回点滅
 *   PIR_LED_TOGGLE センサー反応時の LED 表示を ON/OFF（ON: 2回点滅 / OFF: 1回長く点灯）
 *   PIR_BT_CLEAR   ペアリング情報をすべて消し、両方の LED で5回点滅
 *
 * モードと LED の ON/OFF は settings に保存し、電源を入れ直しても保つ。
 * ボタン操作の点滅は、LED 表示が OFF でも行う（操作が効いたかわかるように）。
 *
 * 設定データの形式（リトルエンディアン、156バイト）:
 *   0   magic       "PIRC"
 *   4   version     2
 *   5   mode_count  2
 *   6   reserved
 *   8   モード1の手順（72バイト）
 *   80  モード2の手順（72バイト）
 *   152 crc         0〜151バイト目の CRC32
 *
 * 手順（72バイト）:
 *   0  count    ステップ数（1〜16）
 *   1  reserved
 *   2  tap_ms   キーを押している時間
 *   4  wait_ms  キーとキーの間の待ち時間
 *   6  reserved
 *   8  steps    &kp に渡す値 × 16。HIDページが0のステップは「追加の待ち時間(ms)」
 */

#define DT_DRV_COMPAT zmk_behavior_pir_switch

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>
#include <drivers/behavior.h>
#include <dt-bindings/zmk/pir_switch.h>
#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
#include <zmk/ble.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1, "&pir は1つだけ定義すること");

#define PIR_NODE DT_DRV_INST(0)
#define MODE_COUNT 2

BUILD_ASSERT(DT_PROP_LEN(PIR_NODE, mode_leds) == MODE_COUNT, "mode-leds は2つ指定すること");

#define PIR_CONFIG_MAGIC 0x43524950 /* "PIRC" */
#define PIR_CONFIG_VERSION 2
#define PIR_CONFIG_MAX_STEPS 16

#define STEP_PAGE(step) (((step) >> 16) & 0xFF)
#define STEP_IS_WAIT(step) (STEP_PAGE(step) == 0)
#define STEP_WAIT_MS(step) ((step) & 0xFFFF)

struct pir_sequence {
    uint8_t count;
    uint8_t reserved0;
    uint16_t tap_ms;
    uint16_t wait_ms;
    uint16_t reserved1;
    uint32_t steps[PIR_CONFIG_MAX_STEPS];
} __packed;

struct pir_config_data {
    uint32_t magic;
    uint8_t version;
    uint8_t mode_count;
    uint16_t reserved;
    struct pir_sequence seq[MODE_COUNT];
    uint32_t crc;
} __packed;

BUILD_ASSERT(sizeof(struct pir_sequence) == 72, "手順のサイズが設定サイトと一致しない");
BUILD_ASSERT(sizeof(struct pir_config_data) == 156, "設定データのサイズが設定サイトと一致しない");

/* ---------- devicetree の値 ---------- */

static const char *const key_press_dev = DEVICE_DT_NAME(DT_PHANDLE(PIR_NODE, key_press));
static const uint8_t config_partition_id =
    DT_FIXED_PARTITION_ID(DT_PHANDLE(PIR_NODE, config_partition));

static const struct gpio_dt_spec mode_leds[MODE_COUNT] = {
    GPIO_DT_SPEC_GET(DT_PHANDLE_BY_IDX(PIR_NODE, mode_leds, 0), gpios),
    GPIO_DT_SPEC_GET(DT_PHANDLE_BY_IDX(PIR_NODE, mode_leds, 1), gpios),
};

static const uint32_t mode1_default_steps[] = DT_PROP(PIR_NODE, mode1_default_keys);
static const uint32_t mode2_default_steps[] = DT_PROP(PIR_NODE, mode2_default_keys);

static const struct {
    const uint32_t *steps;
    size_t count;
} default_sequences[MODE_COUNT] = {
    {mode1_default_steps, ARRAY_SIZE(mode1_default_steps)},
    {mode2_default_steps, ARRAY_SIZE(mode2_default_steps)},
};

/* ---------- 状態 ---------- */

static uint8_t current_mode;    /* 0 = モード1, 1 = モード2 */
static bool led_enabled = true; /* センサー反応時に LED を点けるか */
static bool indicator_on;       /* センサー反応〜次に反応できるまでの間 true */

static struct k_work_delayable indicator_off_work;
static struct k_work_delayable blink_work;
static int blink_remaining; /* 残りの点灯・消灯の切り替え回数 */
static uint8_t blink_mask;  /* 点滅させる LED（ビット0 = モード1, ビット1 = モード2） */
static uint32_t blink_on_ms;
static uint32_t blink_off_ms;

/* ---------- LED ---------- */

static void set_leds(uint8_t mask) {
    for (int i = 0; i < MODE_COUNT; i++) {
        gpio_pin_set_dt(&mode_leds[i], (mask & BIT(i)) ? 1 : 0);
    }
}

/* 点滅中でなければ、センサー反応の表示状態に合わせる */
static void update_leds(void) {
    if (blink_remaining > 0) {
        return;
    }
    set_leds((indicator_on && led_enabled) ? BIT(current_mode) : 0);
}

static void blink_work_handler(struct k_work *work) {
    if (blink_remaining == 0) {
        update_leds();
        return;
    }

    /* 残り回数が偶数なら点灯、奇数なら消灯 */
    bool on = (blink_remaining % 2) == 0;
    set_leds(on ? blink_mask : 0);
    blink_remaining--;
    k_work_reschedule(&blink_work, K_MSEC(on ? blink_on_ms : blink_off_ms));
}

static void start_blink(uint8_t mask, int times, uint32_t on_ms, uint32_t off_ms) {
    blink_mask = mask;
    blink_remaining = times * 2;
    blink_on_ms = on_ms;
    blink_off_ms = off_ms;
    k_work_reschedule(&blink_work, K_NO_WAIT);
}

static void indicator_off_work_handler(struct k_work *work) {
    indicator_on = false;
    update_leds();
}

/* ---------- モード・LED 設定の保存 ---------- */

static void save_state(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
    settings_save_one("pir/mode", &current_mode, sizeof(current_mode));
    settings_save_one("pir/led", &led_enabled, sizeof(led_enabled));
#endif
}

#if IS_ENABLED(CONFIG_SETTINGS)
static int pir_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    if (settings_name_steq(name, "mode", NULL) && len == sizeof(current_mode)) {
        uint8_t mode;
        if (read_cb(cb_arg, &mode, len) == len && mode < MODE_COUNT) {
            current_mode = mode;
        }
        return 0;
    }
    if (settings_name_steq(name, "led", NULL) && len == sizeof(led_enabled)) {
        bool enabled;
        if (read_cb(cb_arg, &enabled, len) == len) {
            led_enabled = enabled;
        }
        return 0;
    }
    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(pir_switch, "pir", NULL, pir_settings_set, NULL, NULL);
#endif

/* ---------- キー送信 ---------- */

static bool load_config(struct pir_config_data *data) {
    const struct flash_area *fa;

    if (flash_area_open(config_partition_id, &fa) != 0) {
        return false;
    }
    int err = flash_area_read(fa, 0, data, sizeof(*data));
    flash_area_close(fa);
    if (err != 0) {
        return false;
    }

    if (data->magic != PIR_CONFIG_MAGIC || data->version != PIR_CONFIG_VERSION ||
        data->mode_count != MODE_COUNT) {
        return false;
    }
    return crc32_ieee((const uint8_t *)data, offsetof(struct pir_config_data, crc)) == data->crc;
}

static void run_sequence(struct zmk_behavior_binding_event *event) {
    struct pir_config_data data;
    const uint32_t *steps;
    size_t count;
    uint32_t tap_ms, wait_ms;

    if (load_config(&data) && data.seq[current_mode].count >= 1 &&
        data.seq[current_mode].count <= PIR_CONFIG_MAX_STEPS) {
        const struct pir_sequence *seq = &data.seq[current_mode];
        steps = seq->steps;
        count = seq->count;
        tap_ms = seq->tap_ms;
        wait_ms = seq->wait_ms;
    } else {
        LOG_INF("設定領域が空または不正なため、既定の手順を使う");
        steps = default_sequences[current_mode].steps;
        count = default_sequences[current_mode].count;
        tap_ms = DT_PROP(PIR_NODE, tap_ms);
        wait_ms = DT_PROP(PIR_NODE, wait_ms);
    }

    struct zmk_behavior_binding kp = {.behavior_dev = key_press_dev};

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
        zmk_behavior_queue_add(event, kp, true, tap_ms);
        zmk_behavior_queue_add(event, kp, false, wait_after);
    }
}

/* ---------- behavior ---------- */

static int on_pir_binding_pressed(struct zmk_behavior_binding *binding,
                                  struct zmk_behavior_binding_event event) {
    switch (binding->param1) {
    case PIR_TRIGGER:
        k_work_cancel_delayable(&indicator_off_work);
        indicator_on = true;
        update_leds();
        run_sequence(&event);
        break;

    case PIR_TEST:
        run_sequence(&event);
        start_blink(BIT(current_mode), 1, 500, 0);
        break;

    case PIR_MODE_1:
    case PIR_MODE_2:
        current_mode = binding->param1 - PIR_MODE_1;
        save_state();
        start_blink(BIT(current_mode), 3, 150, 150);
        break;

    case PIR_LED_TOGGLE:
        led_enabled = !led_enabled;
        save_state();
        if (led_enabled) {
            start_blink(BIT(current_mode), 2, 150, 150);
        } else {
            start_blink(BIT(current_mode), 1, 1000, 0);
        }
        break;

    case PIR_BT_CLEAR:
#if IS_ENABLED(CONFIG_ZMK_BLE)
        zmk_ble_clear_all_bonds();
#endif
        start_blink(BIT(0) | BIT(1), 5, 100, 100);
        break;

    default:
        LOG_ERR("不明な PIR コマンド: %d", binding->param1);
        break;
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_pir_binding_released(struct zmk_behavior_binding *binding,
                                   struct zmk_behavior_binding_event event) {
    /* センサー出力が LOW に戻った。ブロック時間が終わるころに LED を消す */
    if (binding->param1 == PIR_TRIGGER) {
        k_work_reschedule(&indicator_off_work, K_MSEC(DT_PROP(PIR_NODE, indicator_hold_ms)));
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static int behavior_pir_init(const struct device *dev) {
    k_work_init_delayable(&indicator_off_work, indicator_off_work_handler);
    k_work_init_delayable(&blink_work, blink_work_handler);

    for (int i = 0; i < MODE_COUNT; i++) {
        if (!gpio_is_ready_dt(&mode_leds[i])) {
            LOG_ERR("LED の GPIO が使えない");
            return -ENODEV;
        }
        int err = gpio_pin_configure_dt(&mode_leds[i], GPIO_OUTPUT_INACTIVE);
        if (err != 0) {
            return err;
        }
    }
    return 0;
}

static const struct behavior_driver_api behavior_pir_driver_api = {
    .binding_pressed = on_pir_binding_pressed,
    .binding_released = on_pir_binding_released,
};

BEHAVIOR_DT_INST_DEFINE(0, behavior_pir_init, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_pir_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
