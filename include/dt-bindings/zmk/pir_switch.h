/*
 * &pir に渡すコマンド（keymap と src/behavior_pir_switch.c で共用）
 */

#pragma once

#define PIR_TRIGGER 0    /* センサー: 今のモードのキーを送り、次に反応できるまで LED を点ける */
#define PIR_TEST 1       /* 動作確認: 今のモードのキーを送り、モードの色で点滅 */
#define PIR_MODE_1 2     /* モード1（緑）にする */
#define PIR_MODE_2 3     /* モード2（青）にする */
#define PIR_LED_TOGGLE 4 /* センサー反応時の LED 表示を ON/OFF する */
#define PIR_BT_CLEAR 5   /* Bluetooth のペアリング情報をすべて消す */
