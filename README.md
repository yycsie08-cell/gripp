# Gripp

把手機變成遊戲手把。

手機 APP 把按鍵用 WiFi 丟給一塊 ESP32,ESP32 再用藍牙假裝成一支 Xbox 手把。
電腦那邊什麼都不用裝,在藍牙設定裡配對一次就能玩,Windows 和 Steam 都認得。

```
手機 APP ──WiFi(UDP)──▶ ESP32 ──藍牙(Xbox 手把)──▶ 電腦
```

這個 repo 放 **ESP32 韌體**和 **3D 列印外殼**。APP 和造型工坊在網站上:

- 造型工坊、Android APK、網頁直接燒錄:<https://gripp-studio-git.vercel.app>

## 需要準備

- 一塊有 WiFi + 藍牙的 ESP32(ESP32-S2 沒有藍牙,不能用)

  | 板子 | PlatformIO 環境 | 備註 |
  |---|---|---|
  | ESP32-C3 Super Mini | `c3_supermini` | 又小又便宜 |
  | ESP32 32S(NodeMCU-32S) | `esp32_32s` | 便宜的經典款 |
  | ESP32-S3 Super Mini | `s3_supermini` | 已壓到 160 MHz 並降低 WiFi 功率,不會發燙 |
  | 一般 ESP32 DevKit | `esp32dev` | |

- Android 手機,或 iPhone(iPhone 只能用網頁版做造型,不能連板子)

## 燒錄

**最簡單:不用編譯。** 用 Mac 或 Windows 的 Chrome / Edge 打開造型工坊網站,
往下找到「燒錄」區,板子插上電腦、選型號、按燒錄就好。

**自己編譯:** 裝 VS Code + PlatformIO(pioarduino),在 `esp32_ctrl/` 下:

```
tools\pio.bat run -e c3_supermini -t upload
```

也可以雙擊 `esp32_ctrl/pc_tool/開啟控制面板.bat` 用圖形面板選板子燒錄。

燒不進去:按住板子上的 BOOT、點一下 RESET、放開 BOOT,再燒一次。

## 讓板子上網

第一次開機,板子約 15 秒後會開熱點 `Gripp-XXXX`(密碼 `12345678`)。兩種玩法:

- **接家裡 WiFi**:最簡單是用網站上的「WiFi 設定」(Mac / Windows 的 Chrome / Edge,板子插著 USB)。
  也可以手機連熱點,開 `http://192.168.4.1` 選 WiFi。
- **不設定直接玩**:手機一直連著 `Gripp-XXXX` 熱點。

想換 WiFi:長按板子上的 BOOT 鍵 3 秒,設定清掉,回到熱點模式。

## 藍牙配對

在電腦的藍牙設定裡找手把、配對。因為板子是用 XInput(Xbox)模式,
Windows 裡看到的名字會是 **Xbox Wireless Controller**,不是自己取的名字。
這是為了讓 Windows 和 Steam 認得,目前沒辦法同時自訂名字。

測按鍵:Win+R 輸入 `joy.cpl`。

## 3D 列印外殼

`3D/Gripp_GameCase.3mf`(可直接切片列印)和 `.blend`(Blender 原檔),
裡面有 **ESP32-S3 Super Mini** 和 **ESP32-C3 Super Mini** 兩款外殼,各有底座和上蓋。

## 板子上的燈

| 燈 | 意思 |
|---|---|
| 恆亮 | 藍牙已連上電腦 |
| 慢閃 | 等藍牙配對 |
| 快閃 | 熱點模式(還沒連上家裡 WiFi) |

## 封包格式

手機送給板子的封包固定 7 個 byte:

| byte | 內容 |
|---|---|
| 0, 1 | 左搖桿 X、Y(0~254,中心 127) |
| 2, 3 | 右搖桿 X、Y |
| 4 | bit0 右鍵、bit1 下鍵、bit2 上鍵、bit3 左鍵、bit4 L1、bit5 R1、bit6 L2、bit7 R2 |
| 5 | bit0 Select、bit1 Start、bit2 Home、bit3 截圖、bit4~7 十字鍵 上 下 左 右 |
| 6 | bit0 L3、bit1 R3 |

每個 bit 對到手把第幾顆鍵,寫在 `esp32_ctrl/src/main.cpp` 最上面的幾張表。
遊戲裡 A、B 相反之類的問題改那裡就好。

同一個 UDP 連接埠(4444)也收文字指令:`GH:NAME=新名字`、`GH:REBOOT`、`GH:RESETWIFI`。
板子每秒會廣播一次 `ESP32_IP:<IP>;name=…;ble=0|1;fw=…`,控制端聽到就能自動連上。

USB 線連線時用序列埠(115200)收同樣的封包,另外有 `GH:HELLO`、`GH:WIFISCAN`、`GH:WIFISET=ssid<TAB>密碼` 這幾個指令。

## 加一塊清單裡沒有的板子

打開 `esp32_ctrl/platformio.ini`,照抄一段改三個地方:

```ini
[env:my_board]
board = 這塊板子在 PlatformIO 裡的名字
build_flags =
    ${env.build_flags}
    -DGH_LED_PIN=2          ; 板子上的燈接哪一腳,沒有就寫 -1
    -DGH_LED_ACTIVE=HIGH    ; 燈是高電位亮還是低電位亮
    -DGH_BOOT_PIN=0         ; BOOT 鍵接哪一腳
```

## 資料夾

```
esp32_ctrl/   ESP32 韌體(PlatformIO)+ 燒錄控制面板
3D/           外殼(S3、C3)
```

## 已知的限制

- 一塊板子一次只能接一支手機。
- Switch、PS 主機本身不接受這種藍牙手把,「PS 式」「Switch 式」只是 APP 上按鍵的樣子。
