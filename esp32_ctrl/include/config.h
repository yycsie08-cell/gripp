// 常會想改的東西都放這裡。
#pragma once
#include <Arduino.h>

#define FW_VERSION "1.1.0"

// 電腦 / 主機在藍牙清單裡看到的名字。這是「出廠預設」,
// 之後可以在手機 APP 的設定 → 板子,或板子自己的網頁裡改,改完會存在板子裡。
#define PAD_NAME "Gripp Pad"
#define PAD_MAKER "Gripp"

// 板子連不上 WiFi 時自己開的熱點(名字後面會接 4 碼,避免和別人的撞名)
#define AP_PREFIX "Gripp-"
#define AP_PASSWORD "12345678"

// 手機 APP 和板子講話用的 UDP 連接埠
#define UDP_PORT 4444

// 多久沒收到手機的封包就自動放開所有按鍵(毫秒)。
// UDP 會掉封包,沒有這個保護的話角色可能會一直往前走。
#define FAILSAFE_MS 400

// 搖桿中心附近多少算「沒動」(封包值 0~254,中心 127)
#define STICK_DEADZONE 2

// 腳位由 platformio.ini 的 build_flags 帶進來;沒帶就用這裡的預設。
#ifndef GH_LED_PIN
#define GH_LED_PIN -1 // -1 = 不用 LED
#endif
#ifndef GH_LED_ACTIVE
#define GH_LED_ACTIVE HIGH
#endif
#ifndef GH_BOOT_PIN
#define GH_BOOT_PIN 0 // 長按 3 秒清掉 WiFi 設定
#endif
