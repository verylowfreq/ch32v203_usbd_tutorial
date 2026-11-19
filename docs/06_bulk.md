# 06 Bulk 転送（step3_bulk）

step3 の目標：Bulk EP1（OUT 0x01 / IN 0x81、64 バイト）で、データを双方向に流すこと。

- **loopback**：OUT で届いたものを、そのまま IN で返す（FIFO + NAK フロー制御）
- **sink**：OUT を捨てる（PC → デバイスの速度測定）
- **source**：IN でパターンを送り続ける（デバイス → PC の速度測定）

対象ファイル：`step3_bulk/usbd.c`（エンドポイント API）、`step3_bulk/vendor.c`（アプリ）、`step3_bulk/desc.c`

## 6.1 エンドポイントディスクリプタ

```c
9, USB_DESC_INTERFACE, 0, 0, 2 /*NumEndpoints*/, 0xFF, 0x00, 0x00, 0,
7, USB_DESC_ENDPOINT, 0x01, USB_EP_ATTR_BULK, U16_LE(64), 0,   /* EP1 OUT */
7, USB_DESC_ENDPOINT, 0x81, USB_EP_ATTR_BULK, U16_LE(64), 0,   /* EP1 IN  */
```

Full-speed の Bulk の wMaxPacketSize は 8・16・32・64 のどれかです。Bulk では bInterval は無視されます。

## 6.2 エンドポイント表と SET_CONFIGURATION

`usbd.c` では **EPR のインデックス = エンドポイント番号** に固定し、各エンドポイントを表で定義します。

```c
static const ep_def_t ep_defs[] = {
    /* num, type,         OUT(PMA),   IN(PMA),    MPS */
    { 1, EP_TYPE_BULK, PMA_EP1_RX, PMA_EP1_TX, BULK_MPS },
};
```

`SET_CONFIGURATION(1)` を受けたら、BTABLE と EPRn を設定してアプリに通知します。

```c
static void configure_endpoints(bool enable)
{
    for (each d in ep_defs) {
        BT_ADDR_TX(n)  = d->tx_pma;   BT_COUNT_TX(n) = 0;
        BT_ADDR_RX(n)  = d->rx_pma;   BT_COUNT_RX(n) = bt_rx_size(d->mps);
        ep_init(n, d->type | n, EP_STAT_NAK, EP_STAT_NAK);   /* トグル = DATA0、両方向 NAK */
    }
}
...
case USB_REQ_SET_CONFIGURATION:
    usb_configuration = wValue;
    configure_endpoints(usb_configuration != 0);
    vendor_configured(usb_configuration);      /* アプリは usbd_ep_rx_enable() で受信開始 */
```

- **SET_CONFIGURATION でデータトグルは DATA0 に戻ります**（USB 2.0 9.1.1.5）。`ep_init()` で DTOG=0 にしています。
- エンドポイントは NAK で始め、**受け取る準備ができたらアプリが VALID にします**。
  こうすれば、アプリが受け取れる前にホストが送ってくることはありません。

## 6.3 エンドポイント API

```c
void     usbd_ep_write(uint8_t ep_addr, const void *data, uint16_t len); /* IN：PMA ← data、VALID */
uint16_t usbd_ep_read(uint8_t ep_addr, void *dst, uint16_t maxlen);     /* OUT：PMA → dst        */
uint16_t usbd_ep_rx_count(uint8_t ep_addr);                             /* OUT：受信長だけ        */
void     usbd_ep_rx_enable(uint8_t ep_addr);                            /* OUT：VALID（次を受信） */
void     usbd_ep_tx_cancel(uint8_t ep_addr);                            /* IN：NAK（取り消し）    */

/* アプリが実装するコールバック（ISR から呼ばれる） */
void vendor_ep_out(uint8_t ep_addr);       /* OUT が届いた（この時点で STAT_RX は NAK） */
void vendor_ep_in_done(uint8_t ep_addr);   /* IN が ACK された                         */
```

ISR 側：

```c
} else {   /* ep != 0 */
    if (epr & EP_CTR_RX) { ep_clear_ctr_rx(ep); ep_rx_armed[ep] = false; vendor_ep_out(ep); }
    if (epr & EP_CTR_TX) { ep_clear_ctr_tx(ep); ep_tx_armed[ep] = false; vendor_ep_in_done(0x80|ep); }
}
```

**CTR_RX が立った時点で、ハードウェアはすでに STAT_RX=NAK にしています。** アプリは PMA をゆっくり読んでかまいません。
**アプリが `usbd_ep_rx_enable()` を呼ぶまで、次の OUT が PMA を上書きすることはありません**。
これがフロー制御の基本です。（BULK 型と INTERRUPT 型で NAK が効くことは 9 章で実機確認しました。
例外は CONTROL 型だけです。）

## 6.4 ループバックと NAK フロー制御

```
OUT 到着 ─▶ FIFO に格納（8 × 64B）─▶ 空きあり? ─yes─▶ usbd_ep_rx_enable(OUT)（次を受ける）
                                   └─no──▶ NAK のまま（ホストは待つ）
IN が空いていて FIFO が空でない ─▶ usbd_ep_write(IN, FIFO の先頭)
IN 完了 ─▶ FIFO の枠を空ける ─▶ OUT を止めていた? ─▶ usbd_ep_rx_enable(OUT)
```

```c
void vendor_ep_out(uint8_t ep_addr)
{
    n = usbd_ep_read(EP_BULK_OUT, fifo_data[fifo_head], 64);
    fifo_len[fifo_head] = n;  fifo_head = (fifo_head + 1) % FIFO_N;  fifo_count++;
    kick_in();
    if (fifo_count < FIFO_N) usbd_ep_rx_enable(EP_BULK_OUT);
    else                     out_paused = true;      /* 満杯：NAK のまま */
}
void vendor_ep_in_done(uint8_t ep_addr)
{
    fifo_tail = (fifo_tail + 1) % FIFO_N;  fifo_count--;
    if (out_paused) { out_paused = false; usbd_ep_rx_enable(EP_BULK_OUT); }
    kick_in();
}
```

OUT のパケットは、同じ長さの IN パケットとして返します。PC が 100 バイト（64 + 36）送れば、64 + 36 で返ってきます。
PC 側の読み出しは、最後の 36 バイトのショートパケットで完了します。

## 6.5 HALT（CLEAR_FEATURE / SET_FEATURE）

PC 側の `WinUsb_ResetPipe()` や libusb の `clear_halt` は、**CLEAR_FEATURE(ENDPOINT_HALT)** を送ります。
デバイスは (1) **データトグルを DATA0 に戻し**、(2) STALL 状態を解除しなければいけません。

```c
} else {   /* CLEAR_FEATURE */
    ep_halted[n][in] = false;
    if (in) { ep_reset_dtog_tx(n); ep_set_stat_tx(n, ep_tx_armed[n] ? VALID : NAK); }
    else    { ep_reset_dtog_rx(n); ep_set_stat_rx(n, ep_rx_armed[n] ? VALID : NAK); }
}
```

トグルは、**HALT 状態でなくても**戻します（仕様でそう決まっています）。
これをしないと、PC は自分のトグルを DATA0 に戻すのに、デバイスは古いトグルのままになり、**以後パケットが 1 個おきに「重複」として捨てられます**。
`ep_rx_armed/ep_tx_armed`（アプリの希望状態）を覚えておくことで、HALT 解除後に正しい状態へ戻せます。
`GET_STATUS(endpoint)` は bit0 で HALT 状態を返します。

### 実機での確認

奇数個のパケットを往復させ、両側のトグルを DATA1 にした状態から試しました。

```
GET_STATUS before: 0 0
GET_STATUS after SET_FEATURE: 1 1          ← EP 0x01 / 0x81 とも HALT
write while halted -> [Errno 32] Pipe error ← STALL が返る
GET_STATUS after clear_halt: 0 0
loopback after clear_halt: OK               ← トグルが両側で DATA0 にそろった
```

## 6.6 実測結果

```
python host/step3_test.py
```

```
[1] loopback sync: 306 transfers (1..512 B) OK
[2] flow control: write(2048 B) returned 512 after 301 ms (device FIFO = 8 x 64 B), read back 512 B, head match=True
    device stats: rx=576 B, fifo_max=8, rx_paused=1
[3] loopback threaded: 1048576 B OK, 518 KiB/s each way
[4] sink  (PC->dev): 935 KiB/s, device counted 2097152 B
[5] source (dev->PC): 960 KiB/s, pattern errors=0
OK
```

- **[2] フロー制御**：PC が読まずに 2048 バイト書くと、デバイスは FIFO（512 バイト）が埋まった時点で NAK を返します。
  書き込みは **512 バイト送ったところで、300ms 後にタイムアウト**します。読み戻したデータは、送った先頭 512 バイトと一致しました。
  （pyusb の libusb1 バックエンドは、タイムアウトでも例外を出さず、転送できたバイト数を返します。）
- **[3]** ループバックでは片方向約 520 KiB/s です（OUT と IN でバス帯域を分け合うので、合計約 1 MiB/s）。
- **[4][5]** 片方向だけなら **約 935〜960 KiB/s** 出ます。Full-speed Bulk の理論上限は
  1 フレームあたり 19 パケット × 64B = 1216 KB/s です。シングルバッファでは、パケットとパケットの間の CPU 処理
  （ISR → PMA アクセス → 再アーム）のぶん、1 フレームに入るパケット数が減ります。

### 発展：ダブルバッファ

BULK 型のエンドポイントで `EP_KIND=1`（DBL_BUF）にすると、**片方向専用で 2 面のバッファ**になります。USB が片方を使っている間に CPU がもう片方を処理できるので、
理論値に近い速度が出ます（RM 21.2.3）。使用中のバッファは DTOG/SW_BUF で選び、
割り込みは USB_HP に来ます。このチュートリアルでは、単純さを優先して実装していません。

## 6.7 注意：タイムアウトによる「読み捨て」はしない

テストの最初の版では、残ったデータを**タイムアウトになるまで読み捨てて**いました。
この環境（USB 2.0 ハブ経由の FS デバイス）では、**タイムアウトで転送をキャンセルすると、その後デバイスへの制御転送が詰まります**（9 章）。
そこで step3 では、デバイス側の `SET_MODE`（FIFO を空にし、保留中の IN を取り消す）で状態を捨てるようにしました。
