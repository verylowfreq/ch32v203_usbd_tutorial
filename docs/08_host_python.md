# 08 PC 側（Python + pyusb / libusb）とデバッグツール

対象ファイル：`host/ch32vendor.py`（共通ヘルパ）、`host/step*_test.py`、`host/usblog.py`、`host/int_async.py`、`host/exp_ep0_nak.py`

## 8.1 準備

```
pip install pyusb libusb-package      # libusb-package には libusb-1.0.dll が同梱されている
pip install libusb1                   # int_async.py（非同期転送）を使うときだけ
```

Windows の libusb は、**WinUSB ドライバ**が当たったデバイスを直接開けます（だから step2 が大事です）。
管理者権限はいりません。

```python
import usb.core, libusb_package
dev = usb.core.find(idVendor=0x1209, idProduct=0x0001,
                    backend=libusb_package.get_libusb1_backend())
```

## 8.2 基本操作

```python
# ベンダ制御リクエスト（bmRequestType：0xC0 = IN|Vendor|Device、0x40 = OUT|Vendor|Device）
info = dev.ctrl_transfer(0xC0, 0x01, 0, 0, 64)          # GET_INFO
dev.ctrl_transfer(0x40, 0x02, 0, 0, b"hello")           # SET_BUF（OUT データステージ）

# Bulk
dev.write(0x01, data, timeout=1000)
back = dev.read(0x81, len(data), timeout=1000)

# Interrupt
dev.write(0x02, cmd16, timeout=1000)
note = dev.read(0x82, 16, timeout=2000)
```

- WinUSB では `set_configuration()` は不要です。Windows がすでに SET_CONFIGURATION(1) を送っています。
- pyusb は最初の転送のときに、インタフェース 0 を自動で claim します。
- **読み出しサイズは wMaxPacketSize の倍数か、期待する長さぴったり**にしてください。
  残りの空きより大きいパケットが来ると、オーバーフローエラーになります。

## 8.3 タイムアウトの落とし穴（重要）

この環境（USB 2.0 ハブ経由の FS デバイス、Windows + WinUSB + libusb）で観測したことが 2 つあります。

1. **pyusb（libusb1 バックエンド）の Bulk 書き込みは、タイムアウトしても例外を出さないことがあります。** 転送できたバイト数を返します。
   ```python
   n = dev.write(0x01, b"x" * 2048, timeout=300)   # デバイスが NAK → 300ms 後に n == 512
   ```
   戻り値は必ず確認してください。1 バイトも送れなかったとき（Interrupt OUT で NAK が続いた場合など）は `USBTimeoutError` が出ます。

2. **タイムアウトで転送をキャンセルすると、そのデバイスへの制御転送が出なくなります。**
   デバイス側のログを見ると、SETUP がまったく届いていません。**同じ EP で転送を 1 回正常に完了させると**回復します。
   ```python
   # 回復の例（Bulk ループバック）：FIFO が空いた状態で、write と read を 1 往復させる
   dev.write(0x01, b"R" * 64, timeout=1000)
   dev.read(0x81, 64, timeout=1000)
   ```
   したがって、**「タイムアウトするまで読み捨てる」コードは書かないでください**。
   状態を捨てるときは、デバイス側のリクエスト（step3 の `SET_MODE`、step4 の `SET_NOTIFY`）を使います。
   step4 では TICK 通知を裏で流し続けているので、Interrupt IN の読み出しは必ず完了します。

## 8.4 非同期転送（python-libusb1）

`host/int_async.py` は、Interrupt IN の転送を N 本出しておき、完了コールバックの中で出し直します。

```python
def on_done(tr):
    if tr.getStatus() == usb1.TRANSFER_COMPLETED:
        handle(tr.getBuffer()[:tr.getActualLength()])
    tr.submit()                              # すぐに出し直す

for _ in range(N):
    tr = handle.getTransfer()
    tr.setInterrupt(0x82, 16, callback=on_done, timeout=0)
    tr.submit()
while running:
    ctx.handleEventsTimeout(0.1)
```

Interrupt IN を隙間なく受け取るときの定番の方法です。ただしこの環境では、この方法でも 7 章の欠落は減りませんでした
（WinUSB の RAW_IO を有効にした状態では未検証です）。

## 8.5 デバイス内の RAM ログ（usblog.py）

ファームウェアは、RAM 上の 256 エントリのリングバッファ `g_log` にイベントを書きます（`common/dbglog.c`）。
1 エントリは 12 バイト（時刻 [18MHz カウント]、タグ、a、b）です。数十サイクルで済むので、ISR の中でも呼べます。

```
python host/usblog.py build/step2/fw.elf            # 全ログ
python host/usblog.py build/step2/fw.elf --last 30  # 最後の 30 件
python host/usblog.py build/step2/fw.elf --fault    # g_fault（Default_Handler が残した例外情報）
```

仕組み：

1. `riscv-none-elf-nm` で ELF から `g_log` のアドレスを得る。
2. `wlink dump <addr> <len> --out file` でその RAM を読む。
3. `common/dbglog.h` の `LOG_xxx = n` という enum を使ってタグ番号を名前に変え、EPR0 や SETUP を読みやすく表示する。

```
 4   241.364ms SETUP_EPR    EPR0=0xEA60[CTR_RX DTOG_RX=1 RX=NAK SETUP CTRL DTOG_TX=1 TX=NAK EA=0] wLength=64
 5   241.365ms SETUP        IN  bm=0x80 GET_DESCRIPTOR(DEVICE,0) wValue=0x0100 wIndex=0x0000
```

> ⚠ wlink が接続すると、WCH-LinkE が **RCC_CFGR0 を書き換えて** USB が止まります（9 章）。
> ファームウェアの main ループで `system_clock_guard()` がこれを検出して直すので、
> ログを読んだ後も USB は自動で回復します。ガードを外した場合は、`--reset` を付けて読み出し後に MCU をリセットしてください。

## 8.6 Windows 側のその他の確認方法

```powershell
# デバイスの状態とドライバ
Get-PnpDevice -PresentOnly | ? InstanceId -like 'USB\VID_1209*' | fl FriendlyName,Status,Problem,Class
Get-PnpDeviceProperty -InstanceId <InstanceId> -KeyName DEVPKEY_Device_Service

# PnP の履歴（ドライバのマッチング）
Get-WinEvent -LogName 'Microsoft-Windows-Kernel-PnP/Configuration' -MaxEvents 5 | fl

# MS OS ディスクリプタのキャッシュ（VID/PID/bcdDevice ごと）
Get-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Control\usbflags\120900010100
```

USB Device Tree Viewer（UsbTreeView）を使うと、ディスクリプタや BOS / MS OS 2.0 の中身も確認できます。
