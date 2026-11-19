# 03 初期化・バスリセット・EP0 の制御転送

`step1_enum/usbd.c` の `usbd_init()`、割り込みハンドラ、`usbd_bus_reset()`、EP0 ステートマシンを説明します。

## 3.1 初期化手順（usbd_init）

```c
/* 1. PA11/PA12 をプッシュプル出力 Low に（USBD を有効にするまで） */
/* 2. プルアップを切る → 20ms 待つ（ホストに切断を認識させる） */
EXTEN_CTR &= ~(EXTEN_USBD_PU | EXTEN_USBD_LS);
delay_ms(20);

/* 3. USBD のクロックを入れ、ペリフェラルをリセット */
RCC_APB1PCENR |= RCC_USBEN;
RCC_APB1PRSTR |= RCC_USBRST;  RCC_APB1PRSTR &= ~RCC_USBRST;

/* 4. 強制リセット（FRES=1）のまま、パワーダウンを解除（PDWN=0） */
USBD_CNTR = CNTR_FRES;
delay_us(10);                       /* アナログトランシーバの起動時間 */

/* 5. バッファテーブルとアドレス */
USBD_BTABLE = 0;
USBD_DADDR = 0;

/* 6. リセットを解除し、保留中のフラグを消し、割り込みを許可 */
USBD_CNTR = 0;
USBD_ISTR = 0;
USBD_CNTR = CTRM | RESETM | SUSPM | WKUPM | ERRM | PMAOVRM;

/* 7. PFIC で IRQ36 を有効化 */
pfic_enable_irq(IRQN_USB_LP_CAN1_RX0);

/* 8. プルアップを入れる → ホストが接続を検出し、バスリセットを始める */
EXTEN_CTR |= EXTEN_USBD_PU;
```

EP0 の設定は**ここではしません**。USB のバスリセットでエンドポイントレジスタとアドレスはリセットされるので
（RM："the device's address and endpoint registers are reset by a USB reset"）、
これらの設定はすべて RESET 割り込みの中で行います。

## 3.2 割り込みハンドラ：ISTR を回す 1 本のループ

```c
void __attribute__((interrupt)) USB_LP_CAN1_RX0_IRQHandler(void)
{
    for (;;) {
        uint32_t istr = USBD_ISTR;
        if (istr & ISTR_RESET) { ISTR_CLEAR(ISTR_RESET); usbd_bus_reset(); continue; }
        if (istr & ISTR_CTR) {
            unsigned ep = istr & ISTR_EP_ID;
            uint32_t epr = USBD_EPR(ep);
            if (ep == 0) {
                if ((epr & EP_CTR_RX) && (epr & EP_SETUP)) { ...; ep0_on_setup(epr); continue; }
                if (epr & EP_CTR_TX) ep0_on_in();
                if (epr & EP_CTR_RX) ep0_on_out();
            } else { ... }
            continue;
        }
        if (istr & ISTR_SUSP) { USBD_CNTR |= CNTR_FSUSP; ISTR_CLEAR(ISTR_SUSP); continue; }
        if (istr & ISTR_WKUP) { USBD_CNTR &= ~CNTR_FSUSP; ISTR_CLEAR(ISTR_WKUP); continue; }
        if (istr & ISTR_ERR)  { ISTR_CLEAR(ISTR_ERR); continue; }
        if (istr & ISTR_PMAOVR) { ISTR_CLEAR(ISTR_PMAOVR); continue; }
        break;
    }
}
```

- `ISTR.CTR` は、**すべての EPRn の CTR ビットがクリアされると自動で消えます**。ハードウェアが「次に処理すべき EP」を 1 つずつ教えてくれるので、ループで回します。
- 1 つの EP で CTR_RX と CTR_TX が両方立っていれば、両方を処理します。
- **SUSP**：フラグを消すだけだと、すぐにまた発生します（RM："the software must set FSUSP"）。FSUSP をセットしてサスペンド検出を止め、WKUP かバスリセットで解除します。
  このチュートリアルでは低消費電力モード（LPMODE）には入らず、ログに記録するだけです。
- ERR（CRC エラーなど）はホストが自動で再送するので、数えるだけにします。

## 3.3 バスリセットの処理

```c
static void usbd_bus_reset(void)
{
    USBD_BTABLE = 0;
    BT_ADDR_TX(0)  = PMA_EP0_TX;   BT_COUNT_TX(0) = 0;
    BT_ADDR_RX(0)  = PMA_EP0_RX;   BT_COUNT_RX(0) = bt_rx_size(64);

    ep_init(0, EP_TYPE_CONTROL | 0, EP_STAT_VALID, EP_STAT_NAK);   /* EP0：SETUP 待ち */
    for (i = 1; i < 8; i++) ep_init(i, i, EP_STAT_DISABLED, EP_STAT_DISABLED);

    ep0.state = EP0_IDLE;
    usb_configuration = 0;
    USBD_DADDR = DADDR_EF;          /* アドレス 0、機能有効 */
    USBD_CNTR &= ~CNTR_FSUSP;
}
```

EP0 の待機状態は **RX=VALID、TX=NAK** です。
RM（EP_TYPE の説明）に「SETUP を受け取ったときに制御エンドポイントが NAK 状態だと、USBD は応答しない」とあります。
そのため待機中は、いつでも SETUP を受け取れるように RX を VALID（または STALL）にしておきます。

## 3.4 制御転送のステートマシン

```
             SETUP 受信
  ┌───────────────┬──────────────┬───────────────┐
  │ wLength==0    │ IN データ    │ OUT データ    │ エラー
  ▼               ▼              ▼               ▼
STATUS_IN      DATA_IN        DATA_OUT        STALLED
(ZLP を送る)   (≤64B 送る)    (≤64B 受ける)   (両方向 STALL)
  │ CTR_TX        │ CTR_TX       │ CTR_RX          │ 次の SETUP
  │               │ 残りあり?    │ 残りあり?       │
  │               ├─yes→ 次へ    ├─yes→ 次の受信   │
  │               └─no─→ STATUS_OUT (ZLP 待ち)     └─no─→ STATUS_IN
  │                        │ CTR_RX (ZLP)
  ▼                        ▼
 IDLE ◀────────────────── IDLE
 （SET_ADDRESS ならここでアドレスを反映）
```

### SETUP を受け取ったとき（ep0_on_setup）

```c
pma_read(PMA_EP0_RX, &ep0.req, 8);      /* 1. まず 8 バイトを PMA から取り出す */
ep_clear_ctr_rx(0);                     /* 2. CTR_RX をクリア */
ep_set_stat_rxtx(0, NAK, NAK);          /* 3. 両方向を明示的に NAK に */
ok = std_request(&ep0.req) / vendor_control_request(&ep0.req);
```

SETUP を受けたときのハードウェアの動作（RM 21.2.2）：**STAT_RX/DTOG_RX に関係なく SETUP を受け取り、
DTOG_RX=DTOG_TX=1（DATA1）、STAT_RX=STAT_TX=NAK に強制します。**実機でも確認しました。SETUP 時点の EPR0 は常に
`0xEA60` = `CTR_RX DTOG_RX=1 RX=NAK SETUP CTRL DTOG_TX=1 TX=NAK` でした（4 章のログ）。
そのため、データステージ最初のパケットは仕様どおり自動的に DATA1 になります。

> 手順 3（ソフトで NAK に強制）は、RM どおりなら不要ですが、安全のために入れています。
> ただし 9 章で見るとおり、**CH32 では NAK にしても EP0 の OUT データの受理は止まりません。**

リクエストハンドラは、何を送るか / 何を受けるかを指定するだけです。

```c
usbd_ep0_reply(data, len);              /* IN データステージ（wLength で切り詰め） */
usbd_ep0_receive(buf, len, done_cb);    /* OUT データステージ（step2 以降）        */
return true;                            /* false → STALL                           */
```

その後、wLength と方向で状態を決めます。

```c
if (req.wLength == 0)                     ep0_send_status_in();     /* ステータス：ZLP */
else if (req.bmRequestType & 0x80)        { state = DATA_IN;  ep0_send_next_packet(); }
else                                      { state = DATA_OUT; ep_set_stat_rx(0, VALID); }
```

### IN データステージと ZLP

ホストが要求した長さ（wLength）より少なく送る場合、**転送の終わりはショートパケットで示します**。
送る長さが 64 のちょうど倍数で、しかも wLength より短いと、ホストは転送の終わりを判断できません。
そのときは**長さ 0 のパケット（ZLP）を追加**します。

```c
ep0.in_zlp = (len < wLength) && (len % 64 == 0);
...
static void ep0_send_next_packet(void)
{
    n = min(in_left, 64);
    pma_write(PMA_EP0_TX, in_ptr, n);
    BT_COUNT_TX(0) = n;
    if (n < 64) in_zlp = false;            /* ショートパケット（または ZLP）で終わり */
    ep_set_stat_tx(0, VALID);
}
```

CTR_TX（ホストが ACK した）が来たら、残りがあれば（または ZLP が保留中なら）次のパケットを送ります。
なければ **STATUS_OUT** に移り、ホストの ZLP を受け取れるように RX を VALID にします。

### ステータスステージと SET_ADDRESS

**SET_ADDRESS を受け取った瞬間にアドレスを変えてはいけません。**
ステータスステージ（デバイスから ZLP を IN で送る）は、まだ**古いアドレス（0）**で行う必要があります。

```c
case USB_REQ_SET_ADDRESS:
    ep0.pending_addr = wValue & 0x7F;      /* 覚えておくだけ */
    return true;
...
static void ep0_on_in(void) {              /* CTR_TX */
    case EP0_STATUS_IN:
        if (ep0.pending_addr) USBD_DADDR = DADDR_EF | ep0.pending_addr;   /* ここで反映 */
        ep0_go_idle();
}
```

これを間違えると SET_ADDRESS のステータスステージが失敗し、次の（新アドレス宛ての）GET_DESCRIPTOR に応答できなくなります。

### STALL

未対応のリクエスト（たとえば Full-speed 専用デバイスへの DEVICE_QUALIFIER）には、**両方向**で STALL を返します。
STALL 中の EP0 でも次の SETUP は受け取れるので（RM："If the control endpoint is in the STALL state, the SETUP packet will be received correctly"）、
特別な復帰処理を書かなくても、次の SETUP で元に戻ります。

## 3.5 タイミングの読み方

デバイス側のログを見ると、制御転送の各ステージは**約 2ms 間隔**で届いています。

```
SETUP        GET_DESCRIPTOR(STRING,2)
+2024us EP0_IN       left=0 state=DATA_IN      ← IN データステージ完了
+1971us EP0_OUT      count=0 state=STATUS_OUT  ← ステータスステージ完了
```

これは Full-speed デバイスが High-speed ハブ（TT）を経由しているためで、ホストが
トランザクションをそれぞれ別のフレーム / マイクロフレームに割り当てています。異常ではありません。
ISR 内で続けて記録したログの間隔は 0.3〜0.6µs なので、ISR の処理そのものは短く、
ステージ間の 2ms はホスト側のスケジューリングによるものと考えられます。
制御転送 1 回で約 4〜8ms かかるので、step2 の SET_BUF + GET_BUF の往復は約 17〜18ms です。
大量のデータは制御パイプではなく Bulk エンドポイントで送ります（step3）。
