# 07 Interrupt 転送（step4_interrupt）

step4 の目標：Interrupt EP2（OUT 0x02 / IN 0x82、16 バイト、bInterval=1ms）で、**低遅延の通知とコマンド**をやり取りすること。
ここで初めて、**main ループと ISR が同じ USB エンドポイントを共有**します。

対象ファイル：`step4_interrupt/vendor.c`、`step4_interrupt/main.c`、`step4_interrupt/desc.c`、`step4_interrupt/usbd.c`

## 7.1 Interrupt 転送とは

- ホストがエンドポイントを **bInterval ごとに**ポーリングします（Full-speed では 1〜255ms。Windows は 2 のべき乗に丸めます）。
- デバイスにデータがなければ NAK を返し、ホストは次の周期でまた聞きます。**遅延は最大で bInterval です。**
- フロー制御（NAK）、データトグル、HALT は Bulk と同じです。USBD 側の違いは `EP_TYPE=INTERRUPT` だけです。
- Full-speed での最大パケットサイズは 64 バイトです。

```c
7, USB_DESC_ENDPOINT, 0x02, USB_EP_ATTR_INTERRUPT, U16_LE(16), 1,   /* EP2 OUT, 1ms */
7, USB_DESC_ENDPOINT, 0x82, USB_EP_ATTR_INTERRUPT, U16_LE(16), 1,   /* EP2 IN,  1ms */
```

`usbd.c` で変わるのは、**エンドポイント表の 1 行追加だけ**です。

```c
static const ep_def_t ep_defs[] = {
    { 1, EP_TYPE_BULK,      PMA_EP1_RX, PMA_EP1_TX, BULK_MPS },
    { 2, EP_TYPE_INTERRUPT, PMA_EP2_RX, PMA_EP2_TX, INT_MPS },   /* step4 */
};
```

## 7.2 通知のフォーマット（16 バイト）

```c
typedef struct __attribute__((packed)) {
    uint8_t  type;        /* 1 = TICK（周期）、2 = ECHO（EP2 OUT への返事）     */
    uint8_t  seq;         /* 通し番号：飛んでいたら通知が失われている          */
    uint16_t frame;       /* 作成したときの USB フレーム番号（USBD_FNR）      */
    uint32_t uptime_ms;
    uint32_t arg0;        /* TICK：tick 数 / ECHO：受信した長さ               */
    uint32_t arg1;        /* TICK：デバイス側で捨てた数 / ECHO：バイトの合計 */
} notify_t;
```

**通し番号は必ず入れてください。** 受信側が、通知の欠落を必ず検出できるようになります（7.5 参照）。

## 7.3 通知キューと、main と ISR の排他

```
main ループ ─ vendor_poll() ─▶ 周期ごとに notify_push(TICK) ─┐
                                                              ├─▶ キュー（8 段）─▶ EP2 IN
ISR ─ EP2 OUT 受信 ─▶ notify_push(ECHO) ──────────────────────┘        ▲
ISR ─ EP2 IN 完了 ─▶ キューを進める ─▶ 次を送る ──────────────────────────┘
```

キューは ISR と main ループの 2 か所から触ります。このチュートリアルでは単純に、
**main ループがキューを触る間は割り込みを禁止します。**

```c
void vendor_poll(void)          /* main ループ */
{
    if (now - last >= period) {
        uint32_t m = irq_save();                    /* mstatus.MIE = 0 */
        notify_push(NOTIFY_TICK, ++tick_count, notify_dropped);
        irq_restore(m);
    }
}
```

`notify_push()` はキューに積み、IN が空いていれば**その場で EP2 IN をアームします**（`usbd_ep_write()`）。
この関数は EPR2 を読み出し→変更→書き込みするので、ISR が同時に EPR2 を触る可能性があるときに実行してはいけません。
割り込み禁止にすれば、EPR2 を触るのは常に 1 つのコンテキストだけになります。（ハードウェアは EPR を変えることがありますが、2 章の規則で安全です。）

> 割り込み禁止区間を短くしたいなら、PFIC で USB 割り込みだけを止める（`pfic_disable_irq(36)`）か、
> ロックフリーの単一生産者・単一消費者リングにする方法もあります。
> ただし EP0 には「NAK が効かない」問題（9 章）があるので、USB 割り込みを長く止めるのは避けてください。

### EP2 OUT のフロー制御

コマンドが届くと、デバイスは ECHO をキューに積みます。キューが満杯なら **EP2 OUT を NAK のまま**にし、
IN が送り終わって枠が空いたら再開します。Bulk のループバックと同じ仕組みです。

実機での確認：TICK を止めて IN を読まずに EP2 OUT へ書き続けると、**9 回目（キューは 8 段）で書き込みがタイムアウト**しました。
INTERRUPT 型では NAK が効いています。

## 7.4 実測結果

```
python host/step4_test.py
```

```
  EP 0x01 attr=2 mps=64 interval=0
  EP 0x81 attr=2 mps=64 interval=0
  EP 0x02 attr=3 mps=16 interval=1
  EP 0x82 attr=3 mps=16 interval=1
[1] TICK 10ms: received 200, missing 0 (0.0%), device-side drops=0, interval mean 10.00 ms
[1] TICK  1ms: received 600, missing 1198 (66.6%), device-side drops=0, interval mean 1.00 ms
[2] ECHO x200: lost 34, round-trip median 3.99 ms, p95 4.02 ms, max 4.08 ms
[3] bulk loopback 1048576 B OK at 518 KiB/s, TICK(10ms) received 199 missing 0
OK
```

- **10ms 周期の TICK**：欠落なし。間隔も一致しています（フレーム番号の差も 10）。
- **ECHO の往復は約 4ms**：EP2 OUT（次のフレーム）→ ISR 内でキューに積む → EP2 IN（次のフレーム）。
- **Bulk との同時動作**：1MiB のループバック中も、TICK は 10ms 周期で届きます。
- **1ms の TICK と ECHO の一部が失われます** → 次の節。

## 7.5 問題：連続したフレームで IN データがあると、通知が失われる

デバイスが 1ms ごとのポーリングのたびに新しいデータを用意していると、**PC のアプリには 3 個に 1 個しか届きません**。
デバイス側で確認したこと：

- `device-side drops=0`：キューは一度も溢れていないので、**デバイスはすべての通知について ACK を受け取っています**。
- CTR_TX のたびに DTOG_TX を RAM ログに記録すると、**1ms ごとに 0/1 が正しく交互**になっていました。
- データトグルがずれた場合に失われるのは、多くても 1 個おき（50%）です。**2 個続けて失われる（66%）ことは、トグルのずれでは説明できません。**
- HSI の周波数誤差は約 +200ppm で、許容範囲内です。

PC 側の実験：

| 方法 | 欠落率 |
|---|---|
| pyusb 同期読み出し、16B | 66.7% |
| pyusb 同期読み出し、64B（4 パケット分） | 33.3% |
| pyusb 同期読み出し、512B（32 パケット分） | 5.8% |
| libusb 非同期、1 / 2 / 4 本を常に出しておく（`host/int_async.py`） | 66.7% |
| bInterval=4、TICK を 10 / 20ms ごと | 0% |
| bInterval=4、TICK を 5 / 3ms ごと | 50% / 66% |

**読み出しが 1 回完了するたびに、その次の 2 パケットが失われています。**
Bulk（source モード、2MiB）では一度も欠落しませんでした。起きるのは**周期転送（Interrupt）だけ**です。

構成は FS デバイス → USB 2.0 ハブ（Genesys 05E3:0608、シングル TT）→ xHCI です。
デバイスはすべてのパケットについて（TT から）ACK を受け取っているので、**デバイス側に原因は見つかっていません。
ホスト側（TT による周期 split transaction の complete-split の扱い）でデータが捨てられている可能性が高い**と考えていますが、断定はできていません。

未確認の点：

- ルートポートに直結した場合や、別のハブとの比較はまだしていません（9 章）。
- 非同期で 4 本出しても変わりませんでした。ただ、WinUSB の既定設定（RAW_IO なし）では、libusb が複数の転送を出しても、
  ドライバが下位に 1 本ずつしか出していない可能性があります。「読み出しの隙間」が原因という説は、まだ完全には否定できていません。
  WinUSB のパイプポリシー RAW_IO を有効にした状態では試していません。

### 設計上の指針

- **1 回の読み出しを大きくする**と欠落は減ります（512B で 5.8%）。ただし、ショートパケットが来るか
  バッファが埋まるまで読み出しが返らないので、通知の遅延とのトレードオフになります。
- **Interrupt IN は「何かあったという合図。失われることもある」と考えて使う**のが安全です。通し番号で欠落を検出し、
  本当の状態は制御リクエスト（GET_STATUS 相当のベンダリクエスト）で取り直します。
- 失われては困るデータは **Bulk** で送ります（再送の仕組みがあります）。
- デバイスは、PC が読むより速いペースで通知を送らないようにプロトコルを設計します。
