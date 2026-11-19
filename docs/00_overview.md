# 00 概要：K8T6 の USB IP とこのチュートリアルの全体像

## 0.1 目標

- CH32V203K8T6 の USB デバイスコントローラを**レジスタレベルで**使う。
- 独自の **Vendor class** デバイス（`bInterfaceClass = 0xFF`）を作る。
- **Microsoft OS 2.0 ディスクリプタ**で、Windows に INF なしで **WinUSB** を自動適用させる。
- **Bulk**（大量データ）と **Interrupt**（低遅延・周期）のエンドポイントで通信する。
- TinyUSB のような汎用スタックではなく、**チップ・クラス・エンドポイント構成をすべて決め打ち**にする。
  そのぶんコードは小さく、すべての行をリファレンスマニュアルと突き合わせて追えます。

## 0.2 CH32V203K8T6 にある USB IP

CH32V203 ファミリには 2 種類の USB コントローラがあります。

| 名前 | 種類 | 特徴 |
|---|---|---|
| **USBD** | Full-speed デバイス専用 | STM32F1 の「USB FS Device」（FSDEV）とレジスタ互換。CAN と共有の 512B パケットバッファ (PMA) を使う |
| **USBFS**（OTG_FS / USBHD） | Full-speed ホスト / デバイス | WCH 独自設計。バッファは SRAM 上に置き、DMA でアクセスする |

ただし、全品種に両方があるわけではありません。データシートの品種別機能表（CH32V203DS0 V3.0、Table 2-1）を
テキスト抽出したものが以下です。列見出しは 9 個ですが、USB の行は値が 10 個並んでおり、F8x6 が 2 列（TSSOP / QFN）に分かれていると解釈しました。

```
            F6P6 F8x6 F8x6 G6U6 G8R6 K8T6 C6x6 C8x6 RBT6 K8U7
USBD          1    -    1    1    1    1    1    1    1    1
USBHD         -    1    -    -    1    -    1    1    1    1
```

V2.8 の表（K6 列を含む旧版）も同じ並びで、**K8T6 には USBD しかありません**（USBHD は「-」。同じ K8 でも新しい K8U7 には USBHD があります）。
ピン配置図でも、CH32V203K8T6 の PB6/PB7 には `USB2DM/DP` の表記がありません（CxU6 の図には `PB6/USB2DM` がある）。
したがって、使うものは次のとおりです。

- USB IP：**USBD**
- ピン：**PA11 = D-、PA12 = D+**（D+ のプルアップは内蔵。`EXTEN_CTR.USBDPU` で有効化）
- 割り込み：USB_LP（IRQ 36）、USB_HP（IRQ 35）、USBWakeUp（IRQ 58）

> 実測に使ったボードは、wlink 上で `CH32V203C8T6`（ChipID 0x20310500）と表示されました。
> D6 系列（F8/G8/K8/C8）であれば、USBD 部分の挙動は同じです。

## 0.3 最低限の USB 知識（Full-speed）

チュートリアルを読むのに必要なぶんだけ説明します。

### パケットとトランザクション

USB の通信は**すべてホスト主導**です。デバイスは応答するだけです。

```
OUT トランザクション : [OUT トークン] [DATA0/1] [デバイスが ACK/NAK/STALL]
IN  トランザクション : [IN トークン]  [デバイスが DATA0/1 または NAK/STALL] [ホストが ACK]
SETUP               : [SETUP トークン] [DATA0 (8バイト)] [デバイスが ACK]   ← 拒否できない
```

- **NAK** は「まだ準備できていない、後で再送して」という意味です。ホストは自動で再試行します。フロー制御の基本になります。
- **STALL** は「エラー / 未対応のリクエスト」を表します。
- **DATA0/DATA1（データトグル）** は、パケットの重複や欠落を検出する仕組みです。トランザクションが成功するたびに反転します。
  送信側と受信側でトグルがずれると、受信側は「再送」とみなし、ACK を返したうえで**データを捨てます**。

### エンドポイントと転送タイプ

| タイプ | 用途 | このチュートリアル |
|---|---|---|
| Control | 設定・コマンド。必ず EP0 | 標準リクエスト、ベンダリクエスト |
| Bulk | 大量データ。帯域の空きを使う | EP1 IN/OUT（ループバックなど） |
| Interrupt | 周期ポーリング（bInterval ごと） | EP2 IN/OUT（通知） |
| Isochronous | 帯域保証・再送なし | 使わない |

### 制御転送（3 ステージ）

```
SETUP ステージ : SETUP（8バイト：bmRequestType, bRequest, wValue, wIndex, wLength）
データステージ : 0 回以上の IN または OUT（方向は bmRequestType の bit7）
ステータス     : データステージと逆方向のトランザクションを 1 回、長さ 0（ZLP）で
```

### 列挙（Windows がおおまかにやること）

```
バスリセット → GET_DESCRIPTOR(Device, 64) → バスリセット → SET_ADDRESS
→ GET_DESCRIPTOR(Device) → GET_DESCRIPTOR(Config) → GET_DESCRIPTOR(String...)
→ （bcdUSB>=2.01 なら GET_DESCRIPTOR(BOS) → MS OS 2.0 のベンダリクエスト）
→ ドライバ選択 → SET_CONFIGURATION(1)
```

実際の順序は、4 章でデバイス側のログから確認します。

## 0.4 全体構成

```
           ┌──────────── USB_LP 割り込み ────────────┐
 USBD HW → │ ISTR ループ：RESET / CTR / SUSP / WKUP  │
           │   EP0：SETUP → 標準 / ベンダリクエスト  │
           │        IN/OUT → 制御転送ステートマシン  │
           │   EP1/2：CTR → vendor_ep_out / in_done │──→ アプリ (vendor.c)
           └─────────────────────────────────────────┘
 main ループ：system_clock_guard()、vendor_poll()（step4：周期通知）
```

- **USB の処理はすべて 1 本の割り込み（USB_LP）で行います。** USB_HP はダブルバッファと isochronous の
  エンドポイント専用なので、このチュートリアルでは有効にしません。ネストがないので排他も単純です。
- main ループで動くのは step4 の周期通知だけで、割り込み禁止で保護します。

### ステップごとに増えるもの

| step | 追加するもの | 確認すること |
|---|---|---|
| step1_enum | 初期化、バスリセット、EP0 ステートマシン、標準リクエスト、ディスクリプタ | Windows が列挙する（ドライバはまだない） |
| step2_winusb | BOS、MS OS 2.0、制御 OUT のデータステージ、ベンダリクエスト | WinUSB が自動で適用され、pyusb から制御転送できる |
| step3_bulk | 非制御 EP の API、SET_CONFIGURATION、HALT | ループバック、フロー制御、スループット |
| step4_interrupt | Interrupt EP、通知キュー、main ↔ ISR | 周期通知、ECHO、Bulk との同時動作 |

各ステップは単独でビルドできます。前のステップとの差分は
`git diff --no-index step2_winusb step3_bulk` のように比べると分かります。

## 0.5 開発環境

| 項目 | 使ったもの |
|---|---|
| コンパイラ | xpack `riscv-none-elf-gcc` 15.2（`-march=rv32imac_zicsr_zifencei -mabi=ilp32`） |
| ビルド | ninja（`build.ninja` + `common/rules.ninja`） |
| 書き込み | `wlink` 0.1.2 + WCH-LinkE（ファームウェア v2.16） |
| PC 側 | Windows 11、Python 3.14、pyusb 1.3 + libusb-package（libusb-1.0） |
| 資料 | CH32FV2x_V3xRM v2.5（第 21 章 USBD）、CH32V203DS0 V2.8 / V3.0、QingKeV4 Processor Manual v1.5 |

WCH の SDK（EVT）は**まったく使いません**。スタートアップ、リンカスクリプト、レジスタ定義はすべて
`common/` に手書きしたので、チュートリアル中のレジスタ書き込みは必ずコードのどこか 1 行に対応しています。

> コード中のコメントは英語で書いています。
