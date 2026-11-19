# 04 列挙（step1_enum）

step1 の目標：**Windows に USB デバイスとして認識させ**、名前とシリアル番号を読ませること。
ドライバはまだ当てません（step2 で行います）。

対象ファイル：`step1_enum/desc.c`、`step1_enum/usbd.c`（`std_request()`）、`step1_enum/main.c`

## 4.1 ディスクリプタ

### デバイスディスクリプタ（18 バイト）

```c
const uint8_t desc_device[18] = {
    18, USB_DESC_DEVICE,
    U16_LE(0x0200),         /* bcdUSB 2.00（step2 で 2.01 にする）      */
    0x00, 0x00, 0x00,       /* class / subclass / protocol：インタフェースで定義 */
    64,                     /* bMaxPacketSize0                          */
    U16_LE(0x1209),         /* idVendor  (pid.codes)                    */
    U16_LE(0x0001),         /* idProduct (pid.codes のテスト用 PID)      */
    U16_LE(0x0100),         /* bcdDevice：ステップごとに変える           */
    1, 2, 3,                /* iManufacturer / iProduct / iSerialNumber */
    1,                      /* bNumConfigurations                       */
};
```

- **VID/PID**：`1209:0001` は pid.codes の実験用テスト PID です。製品には使わないでください。
- **bcdDevice はステップごとに変えます。** Windows はデバイスについて調べた結果（MS OS ディスクリプタの有無など）を
  VID/PID/bcdDevice 単位でキャッシュします。bcdDevice を変えると、各ステップが別のデバイスとして扱われます（5 章）。
- **bMaxPacketSize0 = 64**：Full-speed の EP0 は 8・16・32・64 から選べます。USBD は 64 に対応しています。

### コンフィギュレーションディスクリプタ（コンフィギュレーション + インタフェース、18 バイト）

```c
9, USB_DESC_CONFIGURATION, U16_LE(18), 1 /*NumInterfaces*/, 1 /*ConfigValue*/, 0,
0x80 /* バスパワー */, 50 /* 100mA */,
9, USB_DESC_INTERFACE, 0, 0, 0 /*NumEndpoints*/, 0xFF /*Vendor*/, 0, 0, 0,
```

インタフェースクラスは **0xFF（Vendor Specific）**です。step1 ではエンドポイントはありません（EP0 だけ）。

### 文字列ディスクリプタ

- インデックス 0：LANGID の一覧（`0x0409` = 英語（米国））。
- 1 Manufacturer / 2 Product：ASCII を UTF-16LE に展開したもの。
- 3 Serial：チップ固有の **96 ビット ID（ESIG 0x1FFFF7E8〜0x1FFFF7F3）**を 16 進 24 桁にしたもの。
  ボードごとにシリアル番号が変わるので、Windows は 1 枚ずつ別のインスタンスとして扱います。

```c
uint32_t id[3] = { ESIG_UNIID1, ESIG_UNIID2, ESIG_UNIID3 };
... hex[(id[w] >> (28 - 4*i)) & 0xF] ...
```

## 4.2 標準リクエスト

| リクエスト | 処理 |
|---|---|
| GET_DESCRIPTOR | DEVICE / CONFIGURATION / STRING（step2 で BOS を追加）。**DEVICE_QUALIFIER は STALL**（Full-speed 専用のため） |
| SET_ADDRESS | pending_addr に保存し、ステータスステージ後に反映（3 章） |
| SET_CONFIGURATION | 0 か 1 を受け付ける（step3 以降はここでエンドポイントを設定） |
| GET_CONFIGURATION | 現在の値を返す（1 バイト） |
| GET_STATUS | デバイス：バスパワー、リモートウェイクアップなし → 0。エンドポイント：HALT 状態 |
| GET/SET_INTERFACE | 代替設定 0 だけ |
| CLEAR/SET_FEATURE | ENDPOINT_HALT（step3 以降）。リモートウェイクアップは未対応（STALL） |

## 4.3 ビルドと書き込み

```
ninja flash-step1
```

## 4.4 Windows がやっていること（実ログ）

`python host/usblog.py build/step1/fw.elf` で、デバイス内の RAM ログを読めます（8 章）。
ボードを初めて接続したときのログです。

```
     0      0.000ms BOOT
     1     23.615ms SUSPEND        ← プルアップを入れた直後。まだ通信がない
     2    153.277ms WAKEUP
     3    153.280ms BUS_RESET    a=0x0001
     4    241.364ms SETUP_EPR    EPR0=0xEA60[CTR_RX DTOG_RX=1 RX=NAK SETUP CTRL DTOG_TX=1 TX=NAK EA=0] wLength=64
     5    241.365ms SETUP        IN  bm=0x80 GET_DESCRIPTOR(DEVICE,0)   ← 最初の要求は 64 バイト
     6    243.373ms EP0_IN       left=0 state=DATA_IN                   ← 18 バイト送る（ショートパケット = 終わり）
     7    245.360ms EP0_OUT      count=0 state=STATUS_OUT
     8    250.359ms BUS_RESET    a=0x0002                               ← Windows がもう一度バスリセット
    10    336.446ms SETUP        OUT bm=0x00 SET_ADDRESS wValue=0x0038
    12    336.932ms EP0_IN       left=0 state=STATUS_IN                 ← ここで DADDR=0x38 を反映
    14    375.432ms SETUP        IN  bm=0x80 GET_DESCRIPTOR(DEVICE,0)   wLength=18
    18    388.445ms SETUP        IN  bm=0x80 GET_DESCRIPTOR(CONFIG,0)   wLength=255
    22    394.451ms SETUP        IN  bm=0x80 GET_DESCRIPTOR(STRING,238) ← 0xEE：MS OS 1.0 の問い合わせ
    23    394.452ms STALL                                               ← 未対応 → STALL
    25    424.479ms SETUP        IN  bm=0x80 GET_DESCRIPTOR(STRING,3)   ← シリアル番号
    29    430.486ms SETUP        IN  bm=0x80 GET_DESCRIPTOR(STRING,0)   ← LANGID
    33    436.491ms SETUP        IN  bm=0x80 GET_DESCRIPTOR(STRING,2)   ← 製品名
    37    442.497ms SETUP        IN  bm=0x80 GET_DESCRIPTOR(DEV_QUALIFIER,0)
    38    442.498ms STALL                                               ← Full-speed 専用 → STALL
    39  10496.930ms SUSPEND                                             ← ドライバがないので Windows がサスペンドさせる
```

注目するところ：

- `SETUP_EPR=0xEA60` は RM と一致しています。**SETUP を受け取った時点で、ハードウェアはすでに RX/TX を NAK、両方の DTOG を 1 にしています。**
- 最初の GET_DESCRIPTOR は wLength=64 です。18 バイト送るとショートパケットになるので、転送はそこで終わります（ZLP は不要）。
- bcdUSB=2.00 だと、Windows は**文字列 0xEE（MS OS 1.0 ディスクリプタ）**を問い合わせます。STALL を返すと「未対応」の意味になります。
  Windows はこの結果を VID/PID/bcdDevice 単位で記録します。実際にこの PC のレジストリ `HKLM\SYSTEM\CurrentControlSet\Control\usbflags\120900010100`
  （`1209` `0001` `0100`）に `osvc = {0, 0}`（MS OS 1.0 なし）が書かれていました。
- ドライバがないので、SET_CONFIGURATION は発行されません。

## 4.5 確認

```powershell
Get-PnpDevice -PresentOnly | ? InstanceId -like 'USB\VID_1209&PID_0001*' | fl FriendlyName,Status,Problem,InstanceId
```

```
FriendlyName : CH32V203 Vendor Step1
Status       : Error
Problem      : CM_PROB_FAILED_INSTALL      ← 列挙はできたが、合うドライバがない
InstanceId   : USB\VID_1209&PID_0001\62AAABCDCB76BCFFE339E339   ← UID から作ったシリアル
```

`CM_PROB_FAILED_INSTALL` は「**ディスクリプタは正しく読めたが、対応するドライバがない**」という意味で、step1 としては想定どおりです。
次の step2 で、Windows に WinUSB を自動で当てさせます。
