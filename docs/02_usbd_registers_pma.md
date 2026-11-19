# 02 USBD レジスタと PMA

USBD の中核である `common/usbd_hw.h` を説明します。どのステップでも同じものを使います。
参照：CH32FV2x_V3xRM 第 21 章「USB Full-speed Device Controller (USBD)」

## 2.1 レジスタマップ

| アドレス | 名前 | 用途 |
|---|---|---|
| 0x40005C00 + 4n | **EPRn**（n=0..7） | エンドポイント n の種類・アドレス・状態・トグル・完了フラグ |
| 0x40005C40 | **CNTR** | 制御：リセット、パワーダウン、割り込みマスク |
| 0x40005C44 | **ISTR** | 割り込み状態（どのイベントか、どの EP か） |
| 0x40005C48 | FNR | フレーム番号、D+/D- のレベル |
| 0x40005C4C | **DADDR** | デバイスアドレス + 機能有効（EF） |
| 0x40005C50 | BTABLE | PMA 内のバッファ記述テーブルの先頭 |
| 0x40006000 | **PMA** | パケットバッファ（512 バイト。CAN と共有） |

実質 16 ビット幅のレジスタですが、WCH SDK と同じく 32 ビットでアクセスします（`REG32`）。

## 2.2 CNTR（制御）

| bit | 名前 | このチュートリアルでの使い方 |
|---|---|---|
| 15 | CTRM | 転送完了割り込みの許可 ✔ |
| 14 | PMAOVRM | PMA オーバーラン割り込みの許可 ✔ |
| 13 | ERRM | エラー割り込みの許可 ✔（数えて無視） |
| 12 | WKUPM | ウェイクアップ割り込みの許可 ✔ |
| 11 | SUSPM | サスペンド割り込みの許可 ✔ |
| 10 | RESETM | バスリセット割り込みの許可 ✔ |
| 9/8 | SOFM/ESOFM | SOF 割り込み（使わない） |
| 3 | FSUSP | サスペンド検出のマスク（**SUSP の後に必ずセット**） |
| 2 | LPMODE | 低消費電力モード |
| 1 | PDWN | パワーダウン（リセット値 1） |
| 0 | FRES | 強制リセット（リセット値 1） |

リセット値は `0x0003`（PDWN=1、FRES=1）です。初期化は「PDWN を解除 → レジスタを設定 → FRES を解除」の順に行います（3 章）。

## 2.3 ISTR（割り込み状態）

| bit | 名前 | アクセス |
|---|---|---|
| 15 | CTR | **読み出し専用**。どれかの EPRn で CTR_RX / CTR_TX が立っている間 1 |
| 14 | PMAOVR | rc_w0 |
| 13 | ERR | rc_w0 |
| 12 | WKUP | rc_w0 |
| 11 | SUSP | rc_w0 |
| 10 | RESET | rc_w0 |
| 9 | SOF | rc_w0 |
| 8 | ESOF | rc_w0 |
| 4 | DIR | 読み出し専用。1 = CTR_RX（OUT/SETUP）が保留中 |
| 3:0 | EP_ID | 読み出し専用。処理すべき EP 番号（番号が小さいほど優先） |

**rc_w0** は「0 を書くとクリア、1 を書いても変化なし」という意味です。1 つのフラグだけを消すには、**そのビットだけ 0、ほかは全部 1** を書きます。

```c
#define ISTR_CLEAR(flag) (USBD_ISTR = (uint16_t)~(flag))
```

`USBD_ISTR &= ~flag`（読み出し→変更→書き込み）と書くと、**読み出してから書き込むまでの間にハードウェアが立てたフラグまで消してしまう**ことがあります。

## 2.4 EPRn：この IP でいちばん難しいところ

```
 15      14      13:12    11     10:9     8      7       6       5:4     3:0
CTR_RX  DTOG_RX  STAT_RX  SETUP  EP_TYPE  KIND  CTR_TX  DTOG_TX  STAT_TX  EA
rc_w0   t        t        r      rw       rw    rc_w0   t        t        rw
```

| アクセス | 0 を書くと | 1 を書くと |
|---|---|---|
| rc_w0（CTR_RX、CTR_TX） | **クリア** | 変化なし |
| t（トグル：DTOG_x、STAT_x） | 変化なし | **反転** |
| rw（EP_TYPE、KIND、EA） | そのまま書き込まれる | そのまま書き込まれる |
| r（SETUP） | — | — |

つまり、**1 つのレジスタに 3 種類の書き込み規則が混在しています**。素直に `EPR |= x` や `EPR &= ~x` と書くと、こうなります。

- **読み出し時に 1 だったトグルビットが、1 のまま書き戻されて反転します。** 状態が気づかないうちに壊れます。
- CTR ビットも、読み出し値をそのまま書き戻すと、0 だった場合に意図しない動作の原因になります。

そこで、書き込む値は毎回ゼロから組み立てる、というルールにします。

```c
w = (r & EP_RW_BITS)         /* rw ビット：読んだ値をそのまま書き戻す          */
  | EP_CTR_RX | EP_CTR_TX    /* CTR：1 = 変化なし（クリアしたいときだけ 0）     */
  | (反転させたいビット);     /* トグルビット：変えたいところだけ 1              */
```

### STAT を目標値にする：XOR で反転するビットを求める

STAT はトグル型のフィールドです。目標値にするには、**現在値 XOR 目標値** が 1 のビットに 1 を書きます。

```c
static inline void ep_set_stat_tx(unsigned ep, unsigned stat)
{
    uint32_t r = USBD_EPR(ep);
    uint32_t w = (r & EP_RW_BITS) | EP_CTR_RX | EP_CTR_TX;
    w |= (r ^ (stat << EP_STAT_TX_SHIFT)) & EP_STAT_TX;
    USBD_EPR(ep) = w;
}
```

| STAT | 意味 |
|---|---|
| 00 DISABLED | トークンを無視する（応答しない） |
| 01 STALL | STALL を返す |
| 10 NAK | NAK を返す（**転送が成功するたびにハードウェアが自動でこの値にする**） |
| 11 VALID | データを受け付ける / 送る |

### CTR のクリア：そのビットだけ 0 を書く

```c
static inline void ep_clear_ctr_rx(unsigned ep)
{
    uint32_t r = USBD_EPR(ep);
    USBD_EPR(ep) = (r & EP_RW_BITS) | EP_CTR_TX;   /* CTR_RX=0 → クリア、CTR_TX=1 → 変化なし */
}
```

### 読み出し→書き込みの間の競合は大丈夫か

読み出してから書き込むまでの間に、ハードウェアが EPR を変えることがあります（トランザクションが完了したとき）。それでも安全な理由は次のとおりです。

- **CTR ビット**：1 を書くので何も起きません。ハードウェアがその間に立てた CTR は消えません。
- **変えるつもりのないトグルビット**：0 を書くので何も起きません。ハードウェアが反転させた値はそのまま残ります。
- **変えるつもりの STAT ビット**：ハードウェアが処理を終えて NAK になっているとき（CTR の後）か、
  ハードウェアが変更しない状況でだけ変更します。たとえば `ep_set_stat_tx(VALID)` は IN が空いているときにしか呼びません。

### エンドポイントの初期化：トグルも DATA0 にそろえる

```c
static inline void ep_init(unsigned ep, uint32_t type_ea, unsigned stat_rx, unsigned stat_tx)
{
    uint32_t r = USBD_EPR(ep);
    uint32_t want = (stat_rx << 12) | (stat_tx << 4);   /* DTOG = 0 */
    uint32_t w = type_ea;                               /* CTR = 0 → クリア */
    w |= (r ^ want) & EP_TOGGLE_BITS;
    USBD_EPR(ep) = w;
}
```

### EP_TYPE と EP_KIND

| EP_TYPE | 種類 | EP_KIND の意味 |
|---|---|---|
| 00 | BULK | DBL_BUF（ダブルバッファ） |
| 01 | CONTROL | STATUS_OUT（ステータスステージで ZLP だけ受け付ける） |
| 10 | ISO | — |
| 11 | INTERRUPT | — |

> ⚠ **CH32 固有の挙動**：CONTROL 型のエンドポイントは、**STAT_RX=NAK でも OUT データを受理します**。
> 9 章で実機確認しました。BULK 型と INTERRUPT 型では NAK が効きます。

## 2.5 PMA（パケットメモリ）と 2x16 アドレッシング

PMA は、USB ブロックと CPU が共有する 512 バイトの専用 RAM です。厄介なのは**アドレスの対応**です。

```
PMA オフセット（USB ブロックから見た値）  CPU アドレス
0x000-0x001 (16bit)                  →  0x40006000
0x002-0x003 (16bit)                  →  0x40006004   ← 16 ビットワードが 4 バイトおきに並ぶ
0x004-0x005 (16bit)                  →  0x40006008
       ...
オフセット off                        →  0x40006000 + off*2
```

これは STM32F1 系の **「2x16 bit」方式**です。各 32 ビットワードの下位 16 ビットだけが実際のメモリです。

```c
static inline volatile uint16_t *pma_word(uint16_t off)
{
    return (volatile uint16_t *)(PMA_BASE + (uint32_t)off * 2u);
}

static inline void pma_write(uint16_t off, const uint8_t *src, uint16_t len)
{
    volatile uint16_t *p = pma_word(off);
    for (i = 0; i + 1 < len; i += 2) {
        *p = src[i] | (src[i+1] << 8);   /* リトルエンディアン、16 ビット書き込み */
        p += 2;                          /* 次のワードは CPU 側で 4 バイト先 */
    }
    if (len & 1) *p = src[len-1];
}
```

WCH SDK の `usb_mem.c`（`UserToPMABufferCopy`）も同じことをしています。
**BTABLE や ADDRn_TX/RX に入れる値はすべて PMA オフセットです。** CPU アドレスを入れてはいけません。

### BTABLE（バッファ記述テーブル）

`USBD_BTABLE` には、PMA 内のテーブルの先頭オフセットを入れます。ここでは 0 にします。
エンドポイント n ごとに 4 つのハーフワード（PMA 上で 8 バイト）があります。

| PMA オフセット | 名前 | 内容 |
|---|---|---|
| BTABLE + n*8 + 0 | ADDRn_TX | IN バッファの PMA オフセット |
| BTABLE + n*8 + 2 | COUNTn_TX | 次の IN で送るバイト数 |
| BTABLE + n*8 + 4 | ADDRn_RX | OUT バッファの PMA オフセット |
| BTABLE + n*8 + 6 | COUNTn_RX | バッファサイズ（上位 6 ビット）+ 受信バイト数（下位 10 ビット） |

> RM では「0x40006000+[BTABLE]+n*16」と書かれていますが、これは同じ場所を CPU 側から見た値（×2）です。

### COUNTn_RX：バッファサイズの符号化

```
15      14:10       9:0
BLSIZE  NUM_BLOCK   COUNT（受信バイト数、読み出し専用）
```

- BLSIZE=0：2 バイト単位。サイズ = NUM_BLOCK × 2（2〜62 バイト）
- BLSIZE=1：32 バイト単位。サイズ = (NUM_BLOCK+1) × 32（32〜512 バイト）

```c
static inline uint16_t bt_rx_size(uint16_t size)
{
    if (size > 62) return 0x8000 | (((size / 32) - 1) << 10);   /* 64B → 0x8400 */
    return ((size + 1) / 2) << 10;                              /* 16B → 0x2000 */
}
```

バッファより大きいデータが届くと、ハードウェアはサイズの上限までしか書かず、STALL を返して PMAOVR を立てます（RM 21.2.2）。

### このチュートリアルの PMA 配置（step4）

```
0x000-0x03F  BTABLE（8 バイト × 8 EP。使うのは 3 つ）
0x040        EP0 OUT  64B      （SETUP と制御 OUT）
0x080        EP0 IN   64B
0x0C0        EP1 OUT  64B      （Bulk）
0x100        EP1 IN   64B
0x140        EP2 OUT  16B      （Interrupt）
0x150        EP2 IN   16B
0x160-0x1FF  空き
```

> PMA は **CAN と共有**です。CAN を使うときは、CAN が上位 128 バイトを使い、USB は下位 384 バイト（0x000〜0x17F）しか使えません（RM 21.2.2）。
> 上の配置は 0x15F までに収まっているので、CAN と併用できます。

## 2.6 DADDR と FNR

- `DADDR = EF(bit7) | ADD[6:0]` です。EF=0 だと USB ブロックは何にも応答しません。
  バスリセット後は `DADDR = EF | 0` を書きます。SET_ADDRESS の後は、**ステータスステージが完了してから** `EF | addr` を書きます（3 章）。
- `FNR` には、最後に受け取った SOF のフレーム番号（11 ビット）と D+/D- のレベル（RXDP/RXDM）が入っています。
  step4 では各通知にフレーム番号を入れ、ホストからタイミングを確認できるようにしています。

## 2.7 割り込み線は 3 本

| IRQ | 名前 | 発生源 |
|---|---|---|
| 36 | USB_LP_CAN1_RX0 | **すべての USB イベント**（CTR、RESET、SUSP、ERR など） |
| 35 | USB_HP_CAN1_TX | isochronous / ダブルバッファ Bulk の CTR だけ |
| 58 | USBWakeUp | サスペンドからの復帰（EXTI ライン） |

このチュートリアルでは isochronous もダブルバッファも使わないので、**USB_LP だけ**を有効にします。
