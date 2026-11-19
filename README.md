# CH32V203 USBD をレジスタから積み上げる：Vendor class + WinUSB + Bulk / Interrupt

CH32V203K8T6 の USB デバイス機能（USBD = ST FSDEV 互換 IP）を、**TinyUSB も WCH の USB ライブラリも使わず**、
レジスタ操作から積み上げて Vendor class + WinUSB（ドライバ自動適用）+ Bulk / Interrupt 転送まで作るチュートリアルです。
チップ決め打ち・USB クラス決め打ちです。すべて実機（水晶なし・HSI クロック、Windows 11、Python + pyusb）で動作と数値を確認しています。

## 構成

```
common/            全ステップ共通（ステップ間で同一）
  ch32v203_regs.h    最小限のレジスタ定義 (RCC/EXTEN/GPIO/PFIC/SysTick/ESIG)
  usbd_hw.h          USBD レジスタ、EPRn 書き込みヘルパ、PMA アクセス
  usb_std.h          USB 2.0 Ch.9 / MS OS 2.0 定数
  startup.S          ベクタテーブル・スタートアップ（HPE 無効）
  link.ld            リンカスクリプト (64K/20K)
  system.c/.h        HSI→PLL 144MHz / USBCLK 48MHz、SysTick 時刻、クロックガード
  dbglog.c/.h        RAM リングログ（wlink dump で読む）
  rules.ninja        ツールチェーンとビルドルール
step1_enum/        Step 1: 列挙まで（ディスクリプタ・標準リクエスト）
step2_winusb/      Step 2: BOS + MS OS 2.0 → WinUSB、ベンダ制御リクエスト（EP0 実験コード入り）
step3_bulk/        Step 3: Bulk EP1 IN/OUT（loopback / sink / source）
step4_interrupt/   Step 4: Interrupt EP2 IN/OUT（通知キュー、main ↔ ISR）
host/              PC 側 Python テスト群
docs/              チュートリアル本文
build.ninja        全ステップのビルド定義
```

## チュートリアル目次

| 章 | 内容 |
|---|---|
| [00 概要](docs/00_overview.md) | K8T6 にある USB IP、USB の最低限、全体構成 |
| [01 起動・クロック・ピン](docs/01_boot_clock_gpio.md) | startup.S / ベクタテーブル / HPE、HSI→PLL 144MHz、USBCLK、プルアップ |
| [02 USBD レジスタと PMA](docs/02_usbd_registers_pma.md) | EPRn の書き方（rc_w0 / toggle）、PMA の 2x16 アドレッシング、BTABLE |
| [03 バスリセットと EP0](docs/03_reset_and_ep0.md) | 初期化、ISR ループ、制御転送ステートマシン、ZLP、SET_ADDRESS |
| [04 列挙 (step1)](docs/04_enumeration.md) | 標準リクエストとディスクリプタ、Windows の列挙を実ログで読む |
| [05 WinUSB (step2)](docs/05_winusb_msos20.md) | bcdUSB 2.01、BOS、MS OS 2.0 ディスクリプタセット、ベンダリクエスト |
| [06 Bulk (step3)](docs/06_bulk.md) | エンドポイント設定、NAK フロー制御、HALT、スループット |
| [07 Interrupt (step4)](docs/07_interrupt.md) | bInterval、通知キュー、main と ISR の排他 |
| [08 PC 側 (Python)](docs/08_host_python.md) | pyusb / libusb、タイムアウトの罠、ログツール |
| [09 実測で分かったハマりどころ](docs/09_pitfalls.md) | **EP0 で NAK が効かない**、WCH-LinkE の RCC 書き換え、ハブ経由のホスト側挙動 |

## ビルド・書き込み・テスト

必要なもの：xpack `riscv-none-elf-gcc`（15.2 で確認）、`ninja`、`wlink`（`C:\DATA-0\bin\wlink.exe`）、
Python 3 + `pip install pyusb libusb-package`（int_async.py を使う場合は `libusb1` も）。

```
ninja                    # 全ステップをビルド → build/stepN/fw.elf, fw.bin, fw.lst, fw.map
ninja flash-step4        # step4 をビルドして wlink で書き込み

python host/step2_test.py      # step2: BOS / MS OS 2.0、ベンダリクエスト 500 往復
python host/exp_ep0_nak.py     # step2: EP0 の NAK 実験（9 章）
python host/step3_test.py      # step3: ループバック、フロー制御、スループット
python host/step4_test.py      # step4: TICK / ECHO 通知、Bulk との同時動作
python host/usblog.py build/step4/fw.elf   # デバイス内の RAM ログを読む
```

## 実測結果（要約）

ボード：CH32V203（wlink の表示は CH32V203C8T6、64KB/20KB）、HSI 8MHz → PLL 144MHz、水晶なし。
PC：Windows 11。USB 2.0 ハブ（Genesys 05E3:0608）経由で接続。

| 項目 | 結果 |
|---|---|
| step1 列挙 | Device / Config / String / Serial（チップ UID）の読み出し OK。ドライバなしなので CM_PROB_FAILED_INSTALL（想定どおり） |
| step2 WinUSB | MS OS 2.0 で WinUSB が自動適用。SET_BUF / GET_BUF 500 往復（0〜256B、マルチパケット）でエラー 0 |
| step3 Bulk | ループバック 1MiB 一致（片方向 約 518 KiB/s）、sink 935 KiB/s、source 950〜960 KiB/s、NAK フロー制御 OK |
| step4 Interrupt | 10ms 周期の TICK は欠落 0、ECHO の往復 約 4ms、OUT のキュー満杯時 NAK を確認。連続フレームで IN データがあると PC 側で欠落する（デバイス側に原因は見つからず、ホスト側が有力・未切り分け。9 章） |
| HSI 精度 | ホストの SOF に対して約 +200ppm（USB FS の許容 ±2500ppm 以内） |

主な発見（詳細は [9 章](docs/09_pitfalls.md)）：

1. **CH32V203 の USBD は、CONTROL 型 EP では STAT_RX=NAK でも OUT データを ACK して受理する。**
   未読の PMA が上書きされる（BULK / INTERRUPT 型では NAK が効く。IN 方向も NAK が効く）。
   → EP0 の OUT データは ISR 内で即座に PMA から読み出す。
2. **wlink が接続すると WCH-LinkE（v2.16）が RCC_CFGR0 を書き換え**、USB が止まる。
   → main ループの `system_clock_guard()` で検出・修復する。
3. ハブ経由の環境では、ホスト側でタイムアウトにより転送をキャンセルすると、その後の制御転送が詰まる。
   → 「タイムアウトまで読み捨てる」処理は使わない。
