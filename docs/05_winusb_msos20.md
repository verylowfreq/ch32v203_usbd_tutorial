# 05 WinUSB：Microsoft OS 2.0 ディスクリプタでドライバを自動適用（step2_winusb）

step2 の目標：

1. 挿すだけで、Windows が **WinUSB** を自動で適用する（INF ファイルも手動インストールも不要）。
2. PC（pyusb）とデバイスで**ベンダ制御リクエスト**をやり取りする（OUT データステージを含む）。

対象ファイル：`step2_winusb/desc.c`（BOS、MS OS 2.0）、`step2_winusb/vendor.c`、`step2_winusb/usbd.c`

## 5.1 仕組み

```
Windows                                       デバイス
  GET_DESCRIPTOR(Device)          ────────▶  bcdUSB = 0x0201  ← 「2.01 以上」は BOS があるという意味
  GET_DESCRIPTOR(BOS)             ────────▶  BOS + Platform Capability (MS OS 2.0)
                                              ├ dwWindowsVersion = 0x06030000 (Win8.1+)
                                              ├ wMSOSDescriptorSetTotalLength = 0xA2
                                              └ bMS_VendorCode = 0x20
  ベンダ IN リクエスト            ────────▶  MS OS 2.0 ディスクリプタセット（162 バイト）
   bmRequestType=0xC0 bRequest=0x20              ├ CompatibleID = "WINUSB"
   wIndex=7                                      └ DeviceInterfaceGUIDs = {1EE17A87-...}
  → winusb.inf の "USB\MS_COMP_WINUSB" に一致して適用される
  → GUID がレジストリに登録される → アプリは GUID でデバイスを探せる
```

## 5.2 bcdUSB = 0x0201

Windows が BOS ディスクリプタを問い合わせるのは、**bcdUSB が 0x0201 以上のときだけ**です。
デバイスディスクリプタの変更はこれだけです。

## 5.3 BOS ディスクリプタ（33 バイト）

```c
const uint8_t desc_bos[5 + 28] = {
    /* BOS ヘッダ */
    5, USB_DESC_BOS, U16_LE(5 + 28), 1,
    /* Platform Capability (Microsoft OS 2.0) */
    28, USB_DESC_DEVICE_CAPABILITY, USB_DEVCAP_PLATFORM, 0,
    /* PlatformCapabilityUUID {D8DD60DF-4589-4CC7-9CD2-659D9E648A9F} */
    0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C,
    0x9C, 0xD2, 0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F,
    U32_LE(0x06030000),          /* dwWindowsVersion：Windows 8.1 以降      */
    U16_LE(0xA2),                /* wMSOSDescriptorSetTotalLength           */
    MS_VENDOR_CODE,              /* bMS_VendorCode（0x20。値は任意）        */
    0,                           /* bAltEnumCode                            */
};
```

**UUID のバイト順に注意**：先頭 3 フィールド（`D8DD60DF`、`4589`、`4CC7`）はリトルエンディアン、
残りは書いてあるとおりに並べます（Microsoft の「混在エンディアン」GUID 表現）。間違えやすいところです。

GET_DESCRIPTOR のハンドラに `USB_DESC_BOS (15)` を追加します。

```c
case USB_DESC_BOS:
    usbd_ep0_reply(desc_bos, desc_bos_len);
    return true;
```

## 5.4 MS OS 2.0 ディスクリプタセット（162 バイト）

単一機能（コンポジットでない）デバイスなら、最小構成は次のとおりです。

```
セットヘッダ (10)                       wTotalLength = 0xA2
├ Compatible ID (20)                    "WINUSB\0\0" + SubCompatibleID 8 バイトの 0
└ レジストリプロパティ (132)            DeviceInterfaceGUIDs (REG_MULTI_SZ)
    wPropertyDataType   = 7 (REG_MULTI_SZ)
    wPropertyNameLength = 42  （"DeviceInterfaceGUIDs\0" の UTF-16）
    PropertyName
    wPropertyDataLength = 80  （"{GUID}\0\0" の UTF-16：38+1+1 文字）
    PropertyData
```

```c
const uint8_t desc_ms_os_20[0xA2] = {
    U16_LE(10), U16_LE(MS_OS_20_SET_HEADER_DESCRIPTOR), U32_LE(0x06030000), U16_LE(0xA2),
    U16_LE(20), U16_LE(MS_OS_20_FEATURE_COMPATIBLE_ID),
    'W','I','N','U','S','B',0,0,  0,0,0,0,0,0,0,0,
    U16_LE(132), U16_LE(MS_OS_20_FEATURE_REG_PROPERTY), U16_LE(7), U16_LE(42),
    W('D'),W('e'),W('v'), ... W('s'),W(0),
    U16_LE(80),
    W('{'),W('1'),W('E'), ... W('}'), W(0), W(0),     /* NUL 2 個：REG_MULTI_SZ の終端 */
};
_Static_assert(sizeof(desc_ms_os_20) == 0xA2, "MS OS 2.0 descriptor set size");
```

- GUID 文字列の **NUL 2 個の終端**がないと、Windows は GUID を登録しません。
- 全体の長さは、セットヘッダの `wTotalLength` と BOS の `wMSOSDescriptorSetTotalLength` の**両方**と一致させます。
  ここでは `_Static_assert` でコンパイル時にチェックしています。
- **コンポジットデバイス**（1 つのデバイスに複数の機能）では、コンフィギュレーションサブセットヘッダと
  ファンクションサブセットヘッダも必要です（このチュートリアルでは扱いません）。

## 5.5 ベンダリクエストと OUT データステージ

MS OS 2.0 の問い合わせに答えるには、EP0 に**ベンダリクエスト**の処理経路が必要です。

```c
switch (req.bmRequestType & USB_REQ_TYPE_MASK) {
case USB_REQ_TYPE_STANDARD: ok = std_request(&req);            break;
case USB_REQ_TYPE_VENDOR:   ok = vendor_control_request(&req); break;   /* step2 */
}
```

```c
bool vendor_control_request(const usb_setup_t *req)
{
    if (req->bRequest == MS_VENDOR_CODE) {
        if (req->wIndex == MS_OS_20_DESCRIPTOR_INDEX /*7*/ && (req->bmRequestType & 0x80)) {
            usbd_ep0_reply(desc_ms_os_20, desc_ms_os_20_len);
            return true;
        }
        return false;
    }
    switch (req->bRequest) {
    case VREQ_GET_INFO: ...   /* IN ：16 バイトの情報ブロック */
    case VREQ_SET_BUF:  ...   /* OUT：最大 256 バイトを保存  */
    case VREQ_GET_BUF:  ...   /* IN ：保存した内容を返す      */
    }
}
```

### 制御 OUT のデータステージ

ハンドラは、**受信先バッファと完了コールバック**を渡すだけです。

```c
case VREQ_SET_BUF:
    usbd_ep0_receive(store, req->wLength, set_buf_done);
    return true;
```

パケットの受信は `usbd.c` が行います。

```c
case EP0_DATA_OUT:
    n = min(count, out_left);
    pma_read(PMA_EP0_RX, out_ptr, n);      /* ← その場ですぐ PMA から読む */
    ep_clear_ctr_rx(0);
    if (out_left == 0 || count < 64) {     /* 完了（長さ到達、またはショートパケット） */
        ok = out_done(&req, out_received);
        ok ? ep0_send_status_in() : ep0_stall();
    } else {
        ep_set_stat_rx(0, VALID);          /* 次のパケットへ */
    }
```

> ⚠ **PMA は ISR 内ですぐに読んでください。** CH32 では CONTROL 型の EP0 が、
> **STAT_RX=NAK のままでも次の OUT パケットを受理します**。読み出しを後回しにすると、次のパケットで PMA が上書きされます（9 章、実測）。
> このチュートリアルの設計方針「すべて ISR 内で遅延なく処理する」が、結果的にこの問題を避けています。

## 5.6 実機で確かめる

```
ninja flash-step2
```

### WinUSB が適用されたか

```powershell
$d = Get-PnpDevice -PresentOnly | ? InstanceId -like 'USB\VID_1209&PID_0001*'
$d | fl FriendlyName,Status,Class
Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_Service,DEVPKEY_Device_DriverInfPath
```

```
FriendlyName : CH32V203 Vendor Step2
Status       : OK
Class        : USBDevice
DEVPKEY_Device_Service       WINUSB
DEVPKEY_Device_DriverInfPath winusb.inf
```

Windows のイベントログ（Kernel-PnP/Configuration）には `Matching Device ID: USB\MS_COMP_WINUSB` と記録されます。
Compatible ID によって一致した、ということです。

### Python からテストする

```
python host/step2_test.py
```

```
found: usb-stack-tutorial / CH32V203 Vendor Step2 / serial 62AAABCDCB76BCFFE339E339
bcdUSB=0x0201 bcdDevice=0x0200
BOS (33 bytes): 05 0F 21 00 01 1C 10 05 00 DF 60 DD D8 89 45 C7 4C 9C D2 65 9D 9E 64 8A 9F 00 00 03 06 A2 00 20 00
MS OS 2.0 set: 162 bytes (vendor code 0x20), compatible ID = WINUSB
GET_INFO: {'magic': 'V203', 'step': 2, 'uptime_ms': 6615, 'set_buf_count': 0}
SET_BUF/GET_BUF x500: errors=0, 18.22 ms/roundtrip
OK
```

500 往復は 0〜256 バイトのランダムな長さで、境界値 0・1・63・64・65・127・128・129・255・256 も含みます。
マルチパケットの OUT データステージ、ZLP、ショートパケットを一通り通しています。

## 5.7 Windows のキャッシュと bcdDevice

Windows は、デバイスが MS OS ディスクリプタを持っているかどうかなどを **VID + PID + bcdDevice** 単位で記録します
（レジストリ `HKLM\SYSTEM\CurrentControlSet\Control\usbflags\VVVVPPPPRRRR`）。
実際にこの PC では、ステップごとに `120900010100`〜`120900010400` のキーができていました。
step1（bcdUSB 2.00）のキーには `osvc = {0,0}`（MS OS 1.0 なし）が記録されています。
同じ VID/PID/bcdDevice のまま開発を続けると、次のような問題が起きることがあります。

- 一度でも列挙に失敗したデバイスは、BOS / MS OS 2.0 を二度と問い合わせてもらえない。
- 一度「ドライバなし」になったデバイスインスタンスが、そのまま残る。

そのため、このチュートリアルでは**ステップごとに bcdDevice を変えています（0x0100 / 0x0200 / 0x0300 / 0x0400）**。
ほかの方法として、上記のキーを削除する（管理者権限が必要）か、デバイスマネージャーでドライバごとデバイスを削除する手もあります。
