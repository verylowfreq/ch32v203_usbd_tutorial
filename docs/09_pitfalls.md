# 09 実測で分かったハマりどころ

チュートリアルを書く過程で、実機で確かめた挙動をまとめます。項目ごとに**再現方法・データ・対策**を示します。

環境：CH32V203（wlink の表示は C8T6、ChipID 0x20310500）、HSI 144MHz、WCH-LinkE v2.16 + wlink 0.1.2、
Windows 11 + WinUSB + libusb、USB 2.0 ハブ（Genesys 05E3:0608）経由で接続。

---

## 9.1 ★ CONTROL 型 EP は、STAT_RX=NAK でも OUT データを ACK する（CH32 固有）

### RM の記述と実際の挙動

RM（および STM32 FSDEV）には「STAT_RX が NAK のとき、OUT トランザクションには NAK を返す。SETUP だけは必ず受け取る」とあります。
CH32V203 の USBD では、**CONTROL 型のエンドポイントがこのとおりに動きません。**

### 実験（step2 + `host/exp_ep0_nak.py`）

step2 のファームウェアには実験モード（`VREQ_EXP_OUT_DELAY`、`g_exp_mode`）があります。

- `EXP_AT_SETUP`：制御 OUT の SETUP を受け取った後、**RX=NAK のまま 3ms 待ってから**データステージを始める。
  待っている間に CTR_RX が立てば、NAK 状態でデータを受理したことになります。
- `EXP_IN_DATA`：データステージで、受け取ったパケットを**3ms 読まずに待つ**（NAK のまま）。
  待っている間に DTOG_RX が動けば、次のパケットを受理して PMA を上書きしたことになります。
- `EXP_GATE_BULK`：待っている間だけ、EP0 の EP_TYPE を BULK にする。
- `EXP_IN_SETUP`：制御 IN で、待っている間 TX=NAK のままにする（IN 方向の確認）。

PC からは 200 バイトの SET_BUF（4 パケット）を送り、GET_BUF と比べます。各条件 8 回です。

```
condition                      wait  mismatch  waits accepted under NAK
At SETUP, CONTROL type        3000us         0      8                  8
At SETUP, BULK gate           3000us         0      8                  0
In data stage, CONTROL type   3000us         8     16                 16
In data stage, BULK gate      3000us         0     32                  0
Control IN, CONTROL type      3000us         0      8                  0
```

ログの例（NAK 中に OUT を受理したところ）：

```
EXPERIMENT a=0xA260 ...     EPR0=[CTR_RX DTOG_RX=0 RX=NAK CTRL DTOG_TX=1 TX=NAK]
EP0_EARLY  b=54038          ← SETUP から 3.0ms 後（18MHz で 54038 カウント）、NAK のまま受理
```

### 結論

| 状況 | 結果 |
|---|---|
| EP0（CONTROL 型）の OUT、STAT_RX=NAK | **受理して ACK する。PMA が上書きされる**（8 回中 8 回データが壊れた） |
| EP0 を一時的に BULK 型にして NAK | NAK が効く |
| EP0 の IN、STAT_TX=NAK | NAK が効く |
| BULK 型 EP（EP1）の OUT、NAK | NAK が効く（step3 のフロー制御で確認） |
| INTERRUPT 型 EP（EP2）の OUT、NAK | NAK が効く（step4：キュー満杯時に 9 回目の書き込みがタイムアウト） |
| SETUP を受け取った時点 | RM どおり、ハードウェアが RX/TX=NAK、DTOG=1 にする（EPR0=0xEA60） |

データが壊れたケースでは、次のパケットが 2〜3ms 後に届き、前のパケットを読む前に PMA を上書きしたため、1 パケット分が失われていました。

### 対策

1. **（このチュートリアルの方法）EP0 の OUT データは、ISR 内ですぐに PMA から読み出す。** NAK でホストを待たせられることに頼らない。
   SETUP もデータパケットも ISR の中で最後まで処理する設計なら、この問題は表に出ません（step2 で 500 往復、エラー 0）。
   **SETUP と OUT データは同じ EP0 の受信バッファを使います。** NAK が効かないので、SETUP を読む前に次の DATA OUT が届くと、
   SETUP そのものが上書きされる危険もあります。**割り込み禁止区間を短くし、USB 割り込みを長く止めない**ことも設計条件です。
2. 読み出しを後回しにしなければならない場合（TinyUSB のように、リクエストをタスクで処理するスタックなど）は、
   **ホストを待たせたい間だけ EP_TYPE を BULK にして**、NAK を効かせます。
   TinyUSB の `dcd_stm32_fsdev.c` は、CH32 向けにまさにこれをしています（`ep0_set_type(U_EP_BULK)`）。
3. 早く届いたパケットを別のバッファに取っておき、データステージを始めた時点で使う方法もあります（同様の考え方のパッチがあります）。

> 以前 TinyUSB で報告された `SET_LINE_CODING` の問題（CDC のコールバックで古いボーレートが見える）は、
> 「データステージの準備が間に合う前にデータを受理していた」という現象でした。今回の実測からは、
> その背景に「CONTROL 型では NAK が効かない」というハードウェアの挙動があると考えられます
> （元の報告は ISR やタスクの処理が間に合わないという観点からの説明です）。

---

## 9.2 ★ wlink が接続すると、WCH-LinkE が RCC_CFGR0 を書き換える

### 症状

- `wlink dump`（ログの読み出し）や `wlink regs` の後に、**USB が動かなくなる**。プルアップを入れ直して再接続しても直らない。リセットすると直る。
- `ninja flash-stepN` の直後、最初のテストがたまに失敗する。

### 調べ方

main ループに、主なレジスタを監視して変化をログに書くコードを一時的に入れました。

```
MARK a=0x0001 b=0x00BC040A   ← 起動時の RCC_CFGR0（PLLMUL=1111：×18）
...
MARK a=0x0101 b=0x00B4040A   ← wlink dump の直後：bit19 がクリアされた（PLLMUL=1101）
```

**RCC_CFGR0 の PLLMUL が変わり**、SYSCLK と USBCLK が 144/48MHz でなくなっていました。
`wlink -vvv` を見ると、wlink がプローブに送っているのは `AttachChip` / `ReadMemory` などの高レベルコマンドだけです。
したがって、書き換えているのは **WCH-LinkE のファームウェア（v2.16）自体**だと考えられます。
`wlink write-mem` で値を書き戻しても直りません（PLL が動いている間は PLLMUL を変更できないため）。

### 対策：クロックガード（`common/system.c`）

```c
bool system_clock_guard(void)
{
    if ((RCC_CFGR0 & CFGR0_CHECK_MASK) == CFGR0_EXPECTED) return false;
    m = irq_save();
    RCC_CFGR0 &= ~RCC_SW_MASK;           /* SYSCLK → HSI            */
    while ((RCC_CFGR0 & RCC_SWS_MASK) != 0) ;
    RCC_CTLR &= ~RCC_PLLON;              /* PLL を止める            */
    while (RCC_CTLR & RCC_PLLRDY) ;
    pll_setup();                         /* ×18、/3、PLL に切り替え */
    irq_restore(m);
    return true;
}
```

すべてのステップで、main ループのたびに呼びます。ログを読んだ後もデバイスは自動で回復し、ホストとの通信は続きます
（ログには `CLOCK_FIX a=1 b=0x00B4040A` が残ります）。
これは開発用のコードです（デバッガをつながない製品では不要です）。

---

## 9.3 HPE が有効なまま `__attribute__((interrupt))` を使わない

WCH の SDK は HPE（ハードウェアによるレジスタ退避）とネストを有効にし（`INTSYSCR=3`）、
WCH 版 GCC 専用の属性 `interrupt("WCH-Interrupt-fast")` を使います。本家 GCC の `__attribute__((interrupt))` は、
**割り込みがネストすると HPE と衝突します**（TinyUSB の ch32v20x BSP は naked ハンドラで回避しています）。
このチュートリアルは startup.S で `INTSYSCR=0`（HPE 無効、ネストなし）にしているので、素の属性で正しく動きます。

---

## 9.4 ホスト側：タイムアウトで転送をキャンセルすると、制御転送が詰まる

### 症状

デバイスが NAK を返し続けている（FIFO 満杯の）Bulk OUT が PC 側でタイムアウトすると、
**その後の制御転送がすべてタイムアウトします**。デバイス側のログでは、**SETUP がまったく届いていません**。
デバイスの EPR0 は待機状態（VALID）で、Bulk IN の読み出しは動きます。
IN の読み出しをタイムアウトでキャンセルした後も同じことが起きます。

### 回復

**同じエンドポイントで転送を 1 回正常に完了させると**、制御転送が通るようになります。

```
write returned 512        ← タイムアウトでキャンセル（デバイスの FIFO が満杯）
read 512                  ← IN を読み出す（デバイスの FIFO が空く）
write64 -> 64 / read -> 64← 正常な往復を 1 回
stats {...}               ← 制御転送が通るようになった
```

### 考察

デバイスには SETUP が届いていないので、デバイス側では直せません。ハブ経由の FS デバイスでは、
ホストが非周期の split transaction をキャンセルしたとき、ハブに `CLEAR_TT_BUFFER` を送る必要があります。
それが行われていないか、ハブが正しく処理していないのではないかと推測しています。
**ルートポートに直結した場合との比較はまだしていません**（要確認）。

### 対策

- PC 側を、タイムアウトによるキャンセルに頼る設計にしない（「タイムアウトまで読み捨てる」をしない）。
- デバイスの状態は、デバイス側のリクエスト（`SET_MODE` / `SET_NOTIFY`）で捨てる。

---

## 9.5 ホスト側の可能性大：連続したフレームで Interrupt IN データがあると欠落する

詳細は 7.5 です。要点：

- デバイスが 1ms ごとのポーリングのたびに新しい IN データを用意していると、アプリには 3 個に 1 個しか届かない。
- デバイスはすべてのパケットについて ACK を受け取り、DTOG も正しく交互、HSI の誤差は約 +200ppm。**デバイス側に原因は見つからない。**
- Bulk では起きない。非同期で複数本出しておいても変わらない（WinUSB の RAW_IO を有効にした状態では未検証）。
- **ハブの TT を経由する周期 split transaction が最有力ですが、未確定です。** ルートポートでの比較はまだです。
- 設計上は、通知に通し番号を入れて欠落を検出し、失われては困るデータは Bulk で送る。1 回の読み出しを大きくすると欠落は減る。

---

## 9.6 その他の細かい点

| 項目 | 内容 |
|---|---|
| ISTR のクリア | `ISTR = ~flag`（消したいビットだけ 0）。`&=` だと、ハードウェアがその間に立てたフラグまで消えることがある |
| EPRn のトグルビット | EPR に `|=` / `&=` を絶対に使わない。必ずヘルパを使う（2 章） |
| SUSP | フラグを消すだけだとすぐ再発する。FSUSP で検出を止め、WKUP かバスリセットで解除する |
| SET_ADDRESS の反映 | ステータスステージが完了してから |
| DEVICE_QUALIFIER | Full-speed 専用デバイスは STALL を返す（Windows は毎回問い合わせる） |
| 文字列 0xEE | bcdUSB=2.00 だと Windows が MS OS 1.0 を問い合わせる。STALL でよい（結果は usbflags に `osvc` としてキャッシュされる） |
| Windows のキャッシュ | VID/PID/bcdDevice 単位でキャッシュされる。ステップごとに bcdDevice を変える |
| PMA と CAN | CAN を使うときは、USB は PMA の下位 384B しか使えない |
| PLLMUL の符号化 | CH32V20x では `1111` = ×18（STM32 では ×16） |
| 制御転送の速度 | ハブ経由では 1 ステージ約 2ms。データは Bulk で送る |
| K8T6 と USBFS | K8T6 には USBD しかない（データシートの機能表）。PB6/PB7 は USB に使えない |

---

## 9.7 まだ確認していないこと（別の環境で再確認したい）

- 9.4 と 9.5（ホスト側）を、**ハブなしでルートポートに直結した場合**、別のハブ、別のホストコントローラで試す。
- 9.5 を、WinUSB のパイプポリシー RAW_IO を有効にして試す。
- 温度による HSI の長期的なずれ（製品では HSE を推奨）。
- ダブルバッファ Bulk（EP_KIND=DBL_BUF）と USB_HP 割り込みでの挙動。
