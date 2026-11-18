// 手機搖桿 — ESP32 端
//
//   手機 APP --WiFi/UDP 或 USB 線--> ESP32 --藍牙 HID--> 電腦 / 主機 / 平板
//
// 封包(手機 → 板子,固定 7 bytes):
//   [0] 左搖桿 X   [1] 左搖桿 Y   [2] 右搖桿 X   [3] 右搖桿 Y   (0~254,中心 127)
//   [4] 按鍵組 1   [5] 按鍵組 2   [6] 按鍵組 3                    (每個 bit 一顆鍵,對照表在下面)
//
// 板子每秒廣播一次自己的位置,APP 聽到就會自動連上:
//   ESP32_IP:192.168.1.23;name=Gripp Pad;ble=1;fw=1.1.0
//
// USB 線(手機用 OTG 接板子,Android 才有)走序列埠 115200,一樣的 7 bytes 外面包一層:
//   0xA5 0x5A <7 bytes> <這 7 bytes 的 XOR>        控制封包
//   GH:HELLO\n、GH:NAME=…\n 等               文字指令,一行一個,跟 UDP 的一樣
// 板子聽到手機後,每秒回報一行  GH:INFO;name=…;ble=1;fw=…\n  讓 APP 知道它在。
//
// APP 也可以送文字指令過來(同一個 UDP 連接埠):
//   GH:NAME=新的名字     改藍牙名稱,存起來後重開機
//   GH:REBOOT            重開機
//   GH:RESETWIFI         清掉 WiFi 設定後重開機
//   GH:WIFISCAN          掃描附近的 WiFi,一個一個回報:GH:WIFI<TAB>訊號<TAB>有沒有密碼<TAB>名稱,最後 GH:WIFIEND
//   GH:WIFISET=名稱<TAB>密碼   試著連這個 WiFi,連得上才存起來並重開機;結果回報 GH:WIFIRES<TAB>OK/FAIL<TAB>說明

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <BleGamepad.h>
#include <NimBLEDevice.h>
#include "config.h"

// ---- 按鍵對照表:封包的 bit → 藍牙手把的第幾顆鍵 ----------------------------
// 想換鍵位只要改這三個表,APP 不用動。0 = 這個 bit 不是一般按鍵(十字鍵另外處理)。
// XInput(Xbox Series X)模式:函式庫裡 BUTTON_1=A、2=B、3=X、4=Y、5=LB、6=RB、7=按左搖桿、8=按右搖桿、
// 9=View(Select)、10=Menu(Start)、11=Xbox 鍵。L2 / R2 在 Xbox 手把是類比扳機,所以不在表裡(applyPacket 另外處理)。
static const uint8_t MAP_B1[8] = {
    BUTTON_2,  // bit0  右邊的鍵 = B
    BUTTON_1,  // bit1  下面的鍵 = A
    BUTTON_4,  // bit2  上面的鍵 = Y
    BUTTON_3,  // bit3  左邊的鍵 = X
    BUTTON_5,  // bit4  L1 = LB
    BUTTON_6,  // bit5  R1 = RB
    0,         // bit6  L2(左扳機,另外處理)
    0,         // bit7  R2(右扳機,另外處理)
};
static const uint8_t MAP_B2[8] = {
    BUTTON_9,  // bit0  Select = View
    BUTTON_10, // bit1  Start = Menu
    BUTTON_11, // bit2  Home = Xbox 鍵
    0,         // bit3  截圖 / Share(Xbox 模式沒有對應的一般鍵)
    0, 0, 0, 0 // bit4~7 十字鍵 上 下 左 右
};
static const uint8_t MAP_B3[8] = {
    BUTTON_7,  // bit0  L3(左搖桿按下)
    BUTTON_8,  // bit1  R3(右搖桿按下)
    0, 0, 0, 0, 0, 0 // bit2 觸控板:Xbox 模式沒有,忽略
};

// XInput 的搖桿是有正負的:-32767 ~ 32767,中心 0
static const int16_t AXIS_MIN = -32767;
static const int16_t AXIS_MAX = 32767;
static const int16_t AXIS_MID = 0;
static const int16_t TRIGGER_MAX = 32767;

BleGamepad *pad = nullptr; // 要先從板子裡讀出名字才能建立,所以放到 setup() 裡
Preferences prefs;
static String padName = PAD_NAME;
WiFiManager wm;
WiFiUDP udp;
WebServer web(80);

static bool portalMode = false;  // true = 還沒連上家裡 WiFi,板子自己開熱點中
static bool webStarted = false;
static bool udpStarted = false;   // UDP 連接埠開了沒(連上 WiFi 或開了熱點才會開)
static uint32_t bootMs = 0;       // setup 結束的時間,用來算「等多久還連不上 WiFi 就開熱點」
static bool radioOn = false;  // WiFi 無線電有沒有開過(USB 模式下掃描 / 設定 WiFi 時才臨時開)
static bool pollSerial(uint8_t *out);
static IPAddress ctrlIp;          // 最近送控制封包來的手機(單播回報用)
static uint32_t ctrlMs = 0;
static bool scanPending = false;  // 非同步掃描 WiFi 中(掃描時主迴圈照跑,不卡住手把)
static bool scanViaSerial = false;
static IPAddress scanIp;
static bool needRestart = false;
static bool padIsNeutral = true;
static uint32_t lastPacketMs = 0;
static bool replyViaSerial = false; // 目前這個指令是從哪裡來的,回覆就送回哪裡
static IPAddress replyIp;
static uint32_t lastSerialMs = 0; // 最近一次從 USB 線收到手機的東西
static uint32_t packetCount = 0;
static String apName;

// ---- 小工具 ----------------------------------------------------------------

// 0 = 沒有藍牙連線;1 = 電腦已連上、可以送按鍵;
// 2 = 電腦有連上,但配對沒完成(多半是電腦留著舊的配對紀錄,要在電腦移除這個裝置再配對一次)
static int bleState() {
    if (pad && pad->isConnected()) return 1;
    NimBLEServer *srv = NimBLEDevice::getServer();
    return (srv && srv->getConnectedCount() > 0) ? 2 : 0;
}

static void led(bool on) {
#if GH_LED_PIN >= 0
    digitalWrite(GH_LED_PIN, on ? GH_LED_ACTIVE : !GH_LED_ACTIVE);
#else
    (void)on;
#endif
}

// 0~254(中心 127)→ 藍牙手把的軸值,中心附近直接當成沒動
static int16_t mapAxis(uint8_t v) {
    const int lo = 127 - STICK_DEADZONE, hi = 127 + STICK_DEADZONE;
    if (v >= lo && v <= hi) return AXIS_MID;
    if (v < lo) return (int16_t)map(v, 0, lo, AXIS_MIN, AXIS_MID);
    if (v > 254) v = 254;
    return (int16_t)map(v, hi, 254, AXIS_MID, AXIS_MAX);
}

static void applyButtons(uint8_t bits, const uint8_t *table) {
    for (uint8_t i = 0; i < 8; i++) {
        if (!table[i]) continue;
        if (bits & (1 << i)) pad->press(table[i]);
        else pad->release(table[i]);
    }
}

static uint8_t hatFrom(uint8_t b2) {
    const bool u = b2 & 0x10, d = b2 & 0x20, l = b2 & 0x40, r = b2 & 0x80;
    if (u && r) return DPAD_UP_RIGHT;
    if (u && l) return DPAD_UP_LEFT;
    if (d && r) return DPAD_DOWN_RIGHT;
    if (d && l) return DPAD_DOWN_LEFT;
    if (u) return DPAD_UP;
    if (d) return DPAD_DOWN;
    if (l) return DPAD_LEFT;
    if (r) return DPAD_RIGHT;
    return DPAD_CENTERED;
}

static void applyPacket(const uint8_t *p) {
    pad->setLeftThumb(mapAxis(p[0]), mapAxis(p[1]));
    pad->setRightThumb(mapAxis(p[2]), mapAxis(p[3]));
    applyButtons(p[4], MAP_B1);
    applyButtons(p[5], MAP_B2);
    applyButtons(p[6], MAP_B3);
    pad->setTriggers((p[4] & 0x40) ? TRIGGER_MAX : 0, (p[4] & 0x80) ? TRIGGER_MAX : 0); // L2 / R2 → 左右扳機
    pad->setHat1(hatFrom(p[5]));
    pad->sendReport();
}

static void releaseEverything() {
    static const uint8_t neutral[7] = {127, 127, 127, 127, 0, 0, 0};
    applyPacket(neutral);
    padIsNeutral = true;
}

// ---- 存在板子裡的設定 --------------------------------------------------------

// 名字最多 24 個 byte(中文一個字 3 byte,約 8 個字),要是正確的 UTF-8,
// 不能有控制字元和 ; = ' " < > &,免得藍牙清單或廣播格式被弄壞
static bool validName(const String &n) {
    const size_t len = n.length();
    if (len < 1 || len > 24) return false;
    size_t i = 0;
    while (i < len) {
        const uint8_t c = (uint8_t)n[i];
        if (c < 0x80) {
            if (c < 0x20 || c == 0x7F || strchr(";='\"<>&", c)) return false;
            i++;
            continue;
        }
        size_t extra;
        if (c >= 0xC2 && c <= 0xDF) extra = 1;
        else if (c >= 0xE0 && c <= 0xEF) extra = 2;
        else if (c >= 0xF0 && c <= 0xF4) extra = 3;
        else return false;
        if (i + extra >= len) return false;
        for (size_t k = 1; k <= extra; k++)
            if (((uint8_t)n[i + k] & 0xC0) != 0x80) return false;
        i += extra + 1;
    }
    return true;
}

static bool saveName(String n) {
    n.trim();
    if (!validName(n)) return false;
    prefs.putString("name", n);
    return true;
}

static void restartSoon() { needRestart = true; }

static IPAddress myIp() {
    if (!radioOn) return IPAddress(0, 0, 0, 0);
    return (WiFi.status() == WL_CONNECTED) ? WiFi.localIP() : WiFi.softAPIP();
}

// ---- 板子自己的小網頁(看狀態用) ------------------------------------------

static void startWeb() {
    web.on("/", HTTP_GET, []() {
        String h = F("<!doctype html><html><head><meta charset='utf-8'>"
                     "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                     "<meta http-equiv='refresh' content='3'><title>Gripp</title>"
                     "<style>body{font-family:sans-serif;background:#15161b;color:#eee;text-align:center;padding:24px}"
                     "p{margin:8px}a{color:#ffc400}</style></head><body><h2>Gripp</h2>");
        h += "<p>韌體 " FW_VERSION "</p>";
        h += "<p>IP:" + myIp().toString() + "</p>";
        {
            const int b = bleState();
            h += String("<p>藍牙:") + (b == 1 ? "已連上主機" : b == 2 ? "有連線但配對沒完成(請在電腦移除裝置再配對)" : "等待配對") + "</p>";
        }
        h += "<p>收到的封包:" + String(packetCount) + "</p>";
        h += "<form action='/set_name' method='get'><p>藍牙名稱 <input name='v' maxlength='24' value='" + padName + "'> <button>儲存</button></p></form>";
        h += F("<p><a href='/reset_wifi'>清掉 WiFi 設定並重開機</a></p></body></html>");
        web.send(200, "text/html", h);
    });
    web.on("/set_name", HTTP_GET, []() {
        if (saveName(web.arg("v"))) {
            web.send(200, "text/plain; charset=utf-8", "名字存好了,板子重開機中。電腦那邊可能要把舊的藍牙裝置移除後重新配對。");
            restartSoon();
        } else {
            web.send(400, "text/plain; charset=utf-8", "名字要 1~24 個英文、數字或一般符號。");
        }
    });
    web.on("/reset_wifi", HTTP_GET, []() {
        web.send(200, "text/plain; charset=utf-8", "WiFi 設定已清除,板子重開機中…");
        delay(300);
        wm.resetSettings();
        ESP.restart();
    });
    web.begin();
    webStarted = true;
}

// ---- 開機 ------------------------------------------------------------------

void setup() {
    Serial.begin(115200);
#if GH_LED_PIN >= 0
    pinMode(GH_LED_PIN, OUTPUT);
#endif
    pinMode(GH_BOOT_PIN, INPUT_PULLUP);
    led(false);

    prefs.begin("gamehand", false);
    padName = prefs.getString("name", PAD_NAME);
    if (!validName(padName)) padName = PAD_NAME;
    pad = new BleGamepad(padName.c_str(), PAD_MAKER, 100);

    BleGamepadConfiguration cfg;
    cfg.setGamepadMode(GamepadMode::XInputSeriesX); // 會自動設好 Xbox 的 VID/PID 和按鍵數,Steam / Windows 才認得;不能再另外改 VID/PID
    cfg.setAutoReport(false); // 一個封包整理完再一次送出,比較省也比較穩
    pad->begin(&cfg);

    // WiFi 在背景連,不卡開機:USB 線、藍牙一開機就能用,
    // 以前這裡會等 WiFi 最久 15 秒,那段時間 APP 完全連不上板子。
    radioOn = true;
    WiFi.mode(WIFI_STA);
    uint8_t mac[6];
    WiFi.macAddress(mac); // 要在 WiFi.mode 之後讀,不然新版核心會讀到全 0
    char tail[5];
    snprintf(tail, sizeof(tail), "%02X%02X", mac[4], mac[5]);
    apName = String(AP_PREFIX) + tail;
    WiFi.setAutoReconnect(true);
    WiFi.setTxPower(WIFI_POWER_11dBm); // 家裡路由器很近,不需要最大功率;發射功率降低,晶片比較不燙
    WiFi.setSleep(WIFI_PS_MIN_MODEM);  // 沒有資料時讓 WiFi 無線電休息(和藍牙共用天線時最省)
    wm.setConfigPortalBlocking(false);
    wm.setSaveConfigCallback([]() { needRestart = true; });
    WiFi.begin(); // 用板子裡存好的 WiFi;沒存過就連不上,15 秒後 netTick() 會開熱點
    bootMs = millis();

    Serial.printf("\n[Gripp] fw %s  名稱 %s\n", FW_VERSION, padName.c_str());
}

// 每一輪主迴圈看一下網路狀態:連上了就開 UDP 和網頁;15 秒還沒連上就開熱點(設定頁)。
// 手機正用 USB 線連著的時候不開熱點,省得和藍牙搶天線。
static void netTick() {
    static uint32_t lastCheck = 0;
    static bool everConnected = false; // 這次開機連上過就不再開熱點(路由器重開機不該讓板子變熱點)
    if (millis() - lastCheck < 250) return;
    lastCheck = millis();
    if (WiFi.status() == WL_CONNECTED) {
        everConnected = true;
        if (!udpStarted) { udp.begin(UDP_PORT); udpStarted = true; }
        if (!portalMode && !webStarted) startWeb();
        return;
    }
    if (!portalMode && !everConnected && millis() - bootMs > 15000 && !(lastSerialMs && millis() - lastSerialMs < 4000)) {
        portalMode = true;
        wm.startConfigPortal(apName.c_str(), AP_PASSWORD); // 非阻塞:開了熱點就馬上回來
        if (!udpStarted) { udp.begin(UDP_PORT); udpStarted = true; }
        Serial.printf("[Gripp] 熱點 %s (密碼 %s),設定頁 http://192.168.4.1\n", apName.c_str(), AP_PASSWORD);
    }
}

// ---- APP 送來的文字指令 ------------------------------------------------------

static void reply(const String &s) {
    if (replyViaSerial) {
        Serial.print("\n" + s + "\n");
    } else {
        udp.beginPacket(replyIp, UDP_PORT);
        udp.print(s);
        udp.endPacket();
    }
}

// 掃描附近的 WiFi 回報給手機(最多 20 個,訊號強的在前面)
static void ensureRadio() {
    if (!radioOn) { WiFi.mode(WIFI_STA); radioOn = true; }
}

static void doScan() {
    ensureRadio();
    if (scanPending) return;
    scanViaSerial = replyViaSerial;
    scanIp = replyIp;
    scanPending = true;
    WiFi.scanNetworks(true, false, false, 120); // 非同步:掃描的 2~3 秒主迴圈照跑,手把不會卡住
}

// 掃完了才回報(在主迴圈裡一直問,不會卡住)
static void finishScan() {
    const int n0 = WiFi.scanComplete();
    if (n0 == WIFI_SCAN_RUNNING) return;
    scanPending = false;
    const bool oldVia = replyViaSerial;
    const IPAddress oldIp = replyIp;
    replyViaSerial = scanViaSerial;
    replyIp = scanIp;
    int n = n0 < 0 ? 0 : n0;
    if (n > 20) n = 20;
    for (int i = 0; i < n; i++) {
        const String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;
        reply("GH:WIFI\t" + String(WiFi.RSSI(i)) + "\t" + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "0" : "1") + "\t" + ssid);
    }
    WiFi.scanDelete();
    reply("GH:WIFIEND");
    replyViaSerial = oldVia;
    replyIp = oldIp;
}

// 先試著連,連得上才存。密碼打錯不會把原本能用的設定蓋掉。
static void doWifiSet(const String &arg) {
    const int t = arg.indexOf('\t');
    const String ssid = t < 0 ? arg : arg.substring(0, t);
    const String pass = t < 0 ? String("") : arg.substring(t + 1);
    if (ssid.length() < 1 || ssid.length() > 32 || pass.length() > 63 || (pass.length() > 0 && pass.length() < 8)) {
        reply("GH:WIFIRES\tFAIL\tWiFi 名稱或密碼長度不對(密碼要 8~63 個字,開放網路就留空)");
        return;
    }
    ensureRadio();
    reply("GH:WIFIRES\tTRY\t連線中…");
    WiFi.persistent(false);
    WiFi.begin(ssid.c_str(), pass.c_str());
    const uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(200);
    if (WiFi.status() == WL_CONNECTED) {
        WiFi.persistent(true);
        WiFi.begin(ssid.c_str(), pass.c_str()); // 同一個設定再來一次,這次會存進板子
        delay(500);
        reply("GH:WIFIRES\tOK\t" + WiFi.localIP().toString());
        restartSoon();
    } else {
        WiFi.disconnect(false);
        WiFi.persistent(true);
        reply("GH:WIFIRES\tFAIL\t連不上,請確認密碼,而且要是 2.4GHz 的 WiFi");
    }
}

static void handleCommand(String cmd) {
    cmd.trim();
    if (!cmd.startsWith("GH:")) return; // 不是給我們的(例如自己的廣播)
    cmd = cmd.substring(3);
    if (cmd.startsWith("NAME=")) {
        if (saveName(cmd.substring(5))) {
            Serial.println("[Gripp] 名字已更新,重開機");
            restartSoon();
        }
    } else if (cmd == "HELLO") {
        if (replyViaSerial) Serial.printf("\nGH:INFO;name=%s;ble=%d;fw=%s\n", padName.c_str(), bleState(), FW_VERSION);
    } else if (cmd == "REBOOT") {
        restartSoon();
    } else if (cmd == "WIFISCAN") { // 注意:指令別剛好 7 個字元,會被當成控制封包
        doScan();
    } else if (cmd.startsWith("WIFISET=")) {
        doWifiSet(cmd.substring(8));
    } else if (cmd == "RESETWIFI") {
        wm.resetSettings();
        restartSoon();
    }
}

// ---- 主迴圈 ----------------------------------------------------------------

static void sendBeacon() {
    String msg = "ESP32_IP:" + myIp().toString() + ";name=" + padName + ";ble=" + String(bleState()) + ";fw=" FW_VERSION;
    if (udpStarted && WiFi.status() == WL_CONNECTED) {
        udp.beginPacket(IPAddress(255, 255, 255, 255), UDP_PORT);
        udp.print(msg);
        udp.endPacket();
    }
    // 廣播在 WiFi 省電模式下常被路由器延遲或丟掉,所以再單播一份給正在操作的手機,
    // APP 才不會因為漏聽幾個廣播就以為斷線。
    if (udpStarted && ctrlMs && millis() - ctrlMs < 10000) {
        udp.beginPacket(ctrlIp, UDP_PORT);
        udp.print(msg);
        udp.endPacket();
    }
    if (udpStarted && portalMode) {
        udp.beginPacket(WiFi.softAPBroadcastIP(), UDP_PORT);
        udp.print(msg);
        udp.endPacket();
    }
    // 手機有接 USB 線的話,也從線上回報一次
    if (lastSerialMs && millis() - lastSerialMs < 4000) {
        Serial.printf("\nGH:INFO;name=%s;ble=%d;fw=%s\n", padName.c_str(), bleState(), FW_VERSION);
    }
}

// 電腦 / 遊戲透過藍牙傳來的震動,轉給手機。強馬達 = 左、弱馬達 = 右,0~255。
// 單播給正在操作的手機,另外廣播一份,USB 線有接的話也從線上送。
static void forwardRumble() {
    if (!pad->isConnected()) return;
    uint8_t l = 0, r = 0;
    bool got = false;
    if (pad->isXInputRumbleReceived()) {
        l = pad->getXInputStrongMotor();
        r = pad->getXInputWeakMotor();
        got = true;
    } else if (pad->isRumbleReceived()) {
        l = pad->getRumbleLeftAmplitude();
        r = pad->getRumbleRightAmplitude();
        got = true;
    }
    static uint8_t lastL = 0, lastR = 0;
    static uint32_t lastMs = 0;
    if (!got || (l == lastL && r == lastR && millis() - lastMs < 200)) return;
    lastL = l;
    lastR = r;
    lastMs = millis();
    char msg[40];
    snprintf(msg, sizeof(msg), "GH:RUMBLE;l=%u;r=%u", l, r);
    if (udpStarted && ctrlMs && millis() - ctrlMs < 10000) {
        udp.beginPacket(ctrlIp, UDP_PORT);
        udp.print(msg);
        udp.endPacket();
    }
    if (lastSerialMs && millis() - lastSerialMs < 4000) Serial.printf("\n%s\n", msg);
}

static void checkBootButton() {
    static uint32_t downSince = 0;
    if (digitalRead(GH_BOOT_PIN) == LOW) {
        if (!downSince) downSince = millis();
        if (millis() - downSince > 3000) {
            Serial.println("[Gripp] 清除 WiFi 設定");
            for (int i = 0; i < 6; i++) { led(i & 1); delay(120); }
            wm.resetSettings();
            ESP.restart();
        }
    } else {
        downSince = 0;
    }
}

static void updateLed() {
    // 恆亮 = 藍牙已連上;慢閃 = 等藍牙配對;快閃 = 熱點 / 設定模式
    const uint32_t now = millis();
    if (pad->isConnected()) led(true);
    else if (portalMode) led((now / 150) & 1);
    else led((now / 600) & 1);
}

// 從 USB 序列埠讀資料:0xA5 0x5A 開頭的是控制封包,其他當成一行行的文字指令。
// 回傳 true = 這一輪有收到新的控制封包(放進 out)。
static bool pollSerial(uint8_t *out) {
    static uint8_t st = 0; // 0 等 0xA5,1 等 0x5A,2 收資料,3 收檢查碼
    static uint8_t buf[7];
    static uint8_t n = 0;
    static char line[128];
    static uint8_t ln = 0;
    bool got = false;
    int guard = 256; // 一輪最多處理這麼多,避免卡住主迴圈
    while (guard-- > 0 && Serial.available() > 0) {
        const uint8_t b = (uint8_t)Serial.read();
        switch (st) {
        case 0:
            if (b == 0xA5) { st = 1; break; }
            if (b == '\n') {
                line[ln] = 0;
                if (ln > 3 && line[0] == 'G' && line[1] == 'H' && line[2] == ':') {
                    lastSerialMs = millis();
                    replyViaSerial = true;
                    handleCommand(String(line));
                }
                ln = 0;
            } else if ((b >= 0x20 || b == '\t') && b != 0x7F && ln < sizeof(line) - 1) {
                line[ln++] = (char)b;
            } else if (ln >= sizeof(line) - 1) {
                ln = 0; // 太長一定不是指令,丟掉
            }
            break;
        case 1:
            st = (b == 0x5A) ? 2 : 0;
            n = 0;
            break;
        case 2:
            buf[n++] = b;
            if (n == 7) st = 3;
            break;
        case 3: {
            uint8_t x = 0;
            for (uint8_t i = 0; i < 7; i++) x ^= buf[i];
            if (x == b) {
                memcpy(out, buf, 7);
                got = true;
                lastSerialMs = millis();
            }
            st = 0;
            ln = 0;
            break;
        }
        }
    }
    return got;
}

void loop() {
    if (portalMode) wm.process();
    if (scanPending) finishScan();
    netTick();
    if (webStarted) web.handleClient();
    if (needRestart) {
        if (pad->isConnected()) releaseEverything(); // 重開機前先把鍵放開
        delay(1200);
        ESP.restart();
    }

    static uint32_t lastBeacon = 0;
    if (millis() - lastBeacon >= 1000) {
        lastBeacon = millis();
        sendBeacon();
    }

    // 把這一輪收到的封包讀完,只套用最新的那一筆
    uint8_t latest[7];
    char text[160];
    bool got = pollSerial(latest);
    int size;
    while (udpStarted && (size = udp.parsePacket()) > 0) {
        if (size == 7) {
            if (udp.read(latest, 7) == 7) {
                got = true;
                ctrlIp = udp.remoteIP();
                ctrlMs = millis();
            }
        } else {
            const int n = udp.read((uint8_t *)text, sizeof(text) - 1); // 讀不完的部分下次 parsePacket 會自動丟掉
            if (n > 3) {
                text[n] = 0;
                replyViaSerial = false;
                replyIp = udp.remoteIP();
                handleCommand(String(text));
            }
        }
    }

    if (got) {
        lastPacketMs = millis();
        packetCount++;
        if (pad->isConnected()) {
            applyPacket(latest);
            padIsNeutral = false;
        }
    } else if (!padIsNeutral && millis() - lastPacketMs > FAILSAFE_MS) {
        if (pad->isConnected()) releaseEverything();
        else padIsNeutral = true;
    }

    forwardRumble();
    checkBootButton();
    updateLed();
    delay(1);
}
