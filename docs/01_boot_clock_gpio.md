# 01 起動・クロック・ピン

USB を動かす前提として、チップを起動し、144MHz で動かし、48MHz の USB クロックを供給します。
対象ファイル：`common/startup.S`、`common/link.ld`、`common/system.c`、`common/ch32v203_regs.h`

## 1.1 スタートアップとベクタテーブル（startup.S）

QingKe V4（RISC-V）は、リセット後に **0x00000000 番地**から実行を始めます。
0 番地には命令が必要なので、ジャンプ命令を置きます。

```asm
    .section .init, "ax", @progbits
_start:
    j       reset_handler
```

### WCH 形式のベクタテーブル

QingKe V4 には独自モードがあり、`mtvec` の下位 2 ビットを `11` にすると、
**ベクタテーブルの各エントリを「ハンドラの絶対アドレス」として読みます**
（標準 RISC-V のベクタモードでは、テーブルにジャンプ命令を並べます）。

```asm
_vector_base:
    .word   _start                      /*  0 */
    ...
    .word   SysTick_Handler             /* 12 */
    ...
    .word   USB_HP_CAN1_TX_IRQHandler   /* 35 */
    .word   USB_LP_CAN1_RX0_IRQHandler  /* 36 */  ← このチュートリアルで使う
    ...
    .word   USBWakeUp_IRQHandler        /* 58 */
```

```asm
    la      t0, _vector_base
    ori     t0, t0, 3          /* mode = 3：ベクタ + 絶対アドレス */
    csrw    mtvec, t0
```

割り込み番号はテーブルのインデックスと一致します。`build/step1/fw.elf` のインデックス 36 に
`USB_LP_CAN1_RX0_IRQHandler` のアドレスが入っていることは、
`riscv-none-elf-objdump -s -j .vector build/step1/fw.elf` で確認できます。

使わない割り込みは `Default_Handler` に飛ばします。`Default_Handler` は `mcause/mepc/mtval` を RAM 変数 `g_fault` に
書いてから停止します。後から `python host/usblog.py <elf> --fault` で原因を調べられます。

### HPE（ハードウェアによるレジスタ退避）と割り込み関数

QingKe V4 には **HPE** という機能があり、caller-saved レジスタをハードウェアで退避・復帰します。
WCH の SDK は HPE を有効にし（`csrw 0x804, 3`）、ハンドラを
`__attribute__((interrupt("WCH-Interrupt-fast")))` で書きます。この属性は WCH 版 GCC にしかありません。

今回使う **xpack の本家 GCC** にはこの属性がありません。
素の `__attribute__((interrupt))` を使うとコンパイラが退避・復帰コードを生成し、
割り込みがネストしたときに HPE と衝突します（TinyUSB の `hw/bsp/ch32v20x/family.c` はこの問題を naked ハンドラで回避しています）。

このチュートリアルでは、いちばん単純な方法を取ります。

```asm
    csrw    0x804, zero        /* INTSYSCR = 0：HPE 無効、ネスト無効 */
```

HPE が無効なら、素の `__attribute__((interrupt))` で正しく動きます。代償は割り込みの出入りが数サイクル増えることだけです。
USB の処理は 1 本の割り込みにまとめるので、ネストも必要ありません。

```c
void __attribute__((interrupt)) USB_LP_CAN1_RX0_IRQHandler(void) { ... }
```

> INTSYSCR のリセット値は 0 です（QingKeV4 マニュアル）。書かなくても結果は同じですが、意図をコードに残すため明示的に書いています。

### startup.S の処理順

1. `gp`（グローバルポインタ）と `sp` を設定する。
2. `.data` をフラッシュから RAM にコピーし、`.bss` をゼロにする。
3. `corecfgr (0xBC0) = 0x1F`（パイプライン / 分岐予測。WCH SDK と同じ値）。
4. `INTSYSCR = 0`。
5. `mtvec` を設定する。
6. `system_init()` → `main()`。

この時点では `mstatus.MIE`（全体の割り込み許可）は 0 のままです。`main()` が USB の初期化を終えてから
`irq_enable_global()` で有効にします。

## 1.2 リンカスクリプト（link.ld）

```
FLASH : 0x00000000, 64K
RAM   : 0x20000000, 20K      （末尾 2KB がスタック）
```

CH32V203 の D6 系列（F8/G8/K8/C8）は、フラッシュ 64KB、SRAM 20KB です。
セクションは `.init`（0 番地）→ `.vector` → `.text` → `.data` → `.bss` の順に並べます。

## 1.3 クロック：HSI 8MHz → PLL 144MHz、USBCLK 48MHz

**USB Full-speed のトランシーバには正確に 48MHz が必要です。** USBCLK は PLL を
/1・/2・/3 で分周して作るので、PLL（= SYSCLK）は **48・96・144MHz** のどれかにします
（データシート注記："When using the USB function, the CPU frequency must be 48MHz or 96MHz or 144MHz"）。

ボードに水晶がないので、クロック源は内蔵の **HSI（8MHz）** です。

```
HSI 8MHz ─(EXTEN_CTR.HSIPRE=1：分周なし)─→ PLL ×18 = 144MHz = SYSCLK = HCLK
                                             └→ USBPRE /3 = 48MHz = USBCLK
APB1 = HCLK/2 = 72MHz（USBD は APB1 上）、APB2 = HCLK/1
```

`common/system.c`（`pll_setup()`）：

```c
EXTEN_CTR |= EXTEN_HSIPRE;          /* HSI を分周せずに PLL へ（PLL 停止中のみ書き込み可） */

cfg &= ~(HPRE|PPRE1|PPRE2|PLLSRC|PLLXTPRE|PLLMUL|USBPRE);
cfg |= RCC_PPRE1_DIV2;              /* APB1 = /2                        */
cfg |= RCC_PLLMUL_18;               /* 0xF<<18：CH32V20x では ×18       */
cfg |= RCC_USBPRE_DIV3;             /* 2<<22  ：USBCLK = PLL/3          */
RCC_CFGR0 = cfg;

RCC_CTLR |= RCC_PLLON;  while (!(RCC_CTLR & RCC_PLLRDY)) ;
RCC_CFGR0 = (RCC_CFGR0 & ~SW) | RCC_SW_PLL;  while ((RCC_CFGR0 & SWS) != SWS_PLL) ;
```

注意点：

- **PLLMUL の符号化は STM32 と違います。** CH32V20x では `1111` が **×18** です（STM32F1 では ×16）。
- **USBPRE は USB クロック（RCC_APB1PCENR.USBEN）を入れる前に設定します。**
- 144MHz でもフラッシュのウェイト設定は不要です（WCH SDK も設定していません）。

### HSI の精度で USB は足りるか

USB FS のデータレートの許容誤差は **±0.25%（±2500ppm）** です。HSI は工場で調整されていますが、水晶のほうが正確です。
試作ボードで、ホストの SOF パケット（正確に 1ms 間隔）から得たフレーム番号と、
HSI 由来のデバイス側 SysTick を比べました。

```
host frames 4989 ms vs device HSI time 4990 ms -> HSI 誤差 約 +200 ppm
```

室温では許容範囲に十分収まっています（1ms 分解能のため、測定には ±200ppm 程度の不確かさがあります）。
製品として温度範囲全体を保証するなら、水晶（HSE）を使うのが安全です。

## 1.4 SysTick による時刻

QingKe V4 の SysTick は **64 ビットのアップカウンタ**です。HCLK/8 = 18MHz で、割り込みなしのフリーランにして使います。

```c
STK_CTLR = 0; STK_CNTL = 0; STK_CNTH = 0;
STK_CTLR = STK_CTLR_STE;        /* STCLK=0 (HCLK/8)、アップカウント、割り込みなし */
```

- `ticks_now()` は下位 32 ビットを返します（18 カウント = 1µs。約 238 秒で一周）。
- `delay_us()` / `delay_ms()` は差分を測って待ちます。
- `millis()` は差分を積算して 32 ビットのミリ秒カウンタにします（238 秒以内ごとに呼ぶこと）。

## 1.5 USB ピンとプルアップ

### GPIO（PA11 = D-、PA12 = D+）

RM 21.2.2 には「USBD を有効にするとピンは自動的にトランシーバへ接続される。
それまでは**プッシュプル出力で Low にしておく**ことを推奨（準備前にホストに検出されないように）」とあります。
これに従います。

```c
RCC_APB2PCENR |= RCC_IOPAEN;
GPIO_OUTDR(GPIOA) &= ~((1<<11)|(1<<12));
/* CFGHR：pin11 = bit[15:12]、pin12 = bit[19:16]。0x3 = プッシュプル出力 50MHz */
```

### 内蔵 1.5kΩ プルアップ（EXTEN_CTR）

Full-speed デバイスは、**D+ を 1.5kΩ で 3.3V に引き上げる**ことでホストに接続を知らせます。
CH32V203 はこの抵抗を内蔵しており、`EXTEN_CTR (0x40023800)` で制御します。

| bit | 名前 | 意味 |
|---|---|---|
| 0 | USBDLS | 1 = Low-speed（プルアップが D- 側になる）/ 0 = Full-speed |
| 1 | USBDPU | 1 = 内蔵プルアップ有効 |
| 4 | HSIPRE | 1 = HSI を分周せずに PLL へ（1.3 で使用） |

`usbd_init()` は、まず**プルアップを切って** 20ms 待ちます。書き込み直後やリセット直後に、
ホストに切断をはっきり認識させるためです。USB ブロックの設定が全部終わってから、**最後にプルアップを入れます**。
その瞬間にホストが接続を検出し、バスリセットを始めます。

## 1.6 割り込みコントローラ（PFIC）

QingKe V4 の PFIC は、割り込み番号ごとに有効化します。

```c
static inline void pfic_enable_irq(unsigned irqn) { PFIC_IENR(irqn >> 5) = 1u << (irqn & 31); }
```

`IENR`（0xE000E100 + 4n）は 1 を書くと有効になります。無効化は別レジスタの `IRER`（0xE000E180 + 4n）で行います。
USB で使うのは `IRQN_USB_LP_CAN1_RX0 = 36` だけです。
全体の許可は `mstatus.MIE`（`csrsi mstatus, 8`）です。
クリティカルセクションでは `irq_save()` / `irq_restore()`（`csrrci`）で MIE を保存・クリア・復元します。
