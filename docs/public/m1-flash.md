# M1 刷机

**仅适用于原装 EMW3080B / MK3080B 模块的 M1。** 本页介绍**保留原 Bootloader** 的首刷流程，要求能够进入庆科 Bootloader，且命令菜单和分区布局与下文一致。

先阅读[刷机前准备](flash-prep)（供电、接线、进入下载模式）——本页只写 M1 特有的步骤。

> EMW3080B 的模块接口在[刷机前准备](flash-prep#emw3080b模块接口m1--a1)中统一说明。**A1 空气净化器请使用[其对应流程](a1-flash)。**

## 1. 串口与分区确认

按准备页接线与供电要求进入 Bootloader。选择 USB-TTL 对应的串口，设置 **921600、8N1、无软硬件流控**。

输入 `?` 确认菜单：

```text
1:FWUPDATE     Update MiCO
2:USERUPDATE   Update user APP
5:MEMORYMAP
6:BOOT
```

输入 `5`，核对以下关键分区。**地址是 Flash 偏移，不是 CPU 映射地址**：

| ID | 分区 | 起始地址 | 长度 |
| --- | --- | --- | --- |
| 2 | Bootloader | `0x00000000` | `0x00013000` |
| 3 | MiCO Kernel | `0x00013000` | `0x000b5000` |
| 6 | User Application | `0x00088000` | `0x00040000` |
| 7 | PARAMETER1 | `0x000c8000` | `0x00004000` |
| 8 | 参数备份（菜单同样显示 PARAMETER1） | `0x000cc000` | `0x00004000` |

MiCO 区域包含用户应用，应用相对 MiCO 起点的偏移为 `0x75000`。**若菜单或布局不同，停止本流程。**

本方法不写 Bootloader、参数、ATE、OTA Storage 和 USER Storage；但覆盖内核或应用仍会替换原固件，**不能以保留这些区域代替原厂备份**。

## 2. 准备正确的文件

下载同一版本的 M1 固件与 `SHA256SUMS`，校验所用文件。

> **不要将 `.all.bin` 发送给 `1` 或 `2`，也不要将未经转换的 `.ota.bin` 发送给 `2`。** `.all.bin` 含 Bootloader 等区域，只用于完整地址烧录流程。

本项目当前 M1 `.ota.bin` 的结构为：内核及填充 `0x75000` 字节 + 用户应用 + 末尾 16 字节 MD5。

- 串口 `FWUPDATE` 使用**去掉 MD5 尾部**后的连续内核与应用数据。
- `USERUPDATE` 使用带 8 字节长度/CRC 头的 `.usr.bin`。

若 Release 未单独提供 `.usr.bin`，可从校验通过的 `.ota.bin` 提取。下面在固件所在目录运行，检查长度、MD5、应用魔数、ABI 和 CRC 后才生成两个串口文件：

```sh
python3 - <<'PY'
from pathlib import Path
import binascii
import hashlib
import struct

image = Path('phicomm_m1.ota.bin').read_bytes()
offset = 0x75000
assert offset + 144 <= len(image) <= offset + 0x40000 + 16, '镜像长度异常'
assert hashlib.md5(image[:-16]).digest() == image[-16:], 'OTA MD5 不符'
length, reserved, crc, magic, stack, abi = struct.unpack_from('<IHHIII', image, offset)
assert 120 <= length <= 0x40000 - 8, '应用长度异常'
assert len(image) == offset + 8 + length + 16, '应用边界异常'
assert magic == 0xc89346 and abi == 3, '应用魔数或 ABI 不符'
user = image[offset:-16]
assert reserved == crc == binascii.crc_hqx(user[8:], 0), '应用 CRC 不符'
assert b'PHICOMM_M1_EMW3080\0' in user, '非 M1 镜像'
Path('m1-fw.serial.bin').write_bytes(image[:-16])
Path('m1-user.serial.bin').write_bytes(user)
print('内核+应用:', len(image) - 16, 'bytes')
print('用户应用:', len(user), 'bytes; SHA256:', hashlib.sha256(user).hexdigest())
PY
```

生成后**记录脚本输出的文件长度**，发送完成时核对设备显示的 `Size`。文件长度与校验值随固件变化，请使用所下载文件的实际结果。

## 3. 发送与回读

1. 在 `MXCHIP>` 输入 `1` 并回车，等待 `Waiting for the file to be sent` / `C`，使用 **YMODEM（CRC、1 KiB 数据包）**发送 `m1-fw.serial.bin`。建议使用支持包间隔的发送工具，**将数据包间隔设为 50 ms**。写入是否正确仍需通过回读校验确认。

2. 等待设备打印 `Successfully!`，核对 `Size`。

   > 该提示只代表 Bootloader 报告接收/写入完成，**不能据此宣布应用可启动**。

3. 输入 `2 -r`，使用串口工具的 **Receive YMODEM** 接收用户分区，保存为 `USER.bin`。回读文件应为整个 `0x40000` 分区，即 **262144 bytes**。

   Linux 脚本需要独占串口时，先用 **Ctrl-A、Q** 退出 minicom，再运行脚本；**不要让两个程序同时读写同一端口。**

4. 将回读文件前 `m1-user.serial.bin` 长度的内容与期望应用逐字节比较，同时验证应用 CRC：

   ```sh
   python3 - <<'PY'
   from pathlib import Path
   import binascii
   import hashlib
   import struct

   expected = Path('m1-user.serial.bin').read_bytes()
   readback = Path('USER.bin').read_bytes()
   assert len(readback) == 0x40000, '用户分区回读长度不符'
   actual = readback[:len(expected)]
   assert actual == expected, '应用回读不一致，不要启动；重新写入并回读'
   length, reserved, crc = struct.unpack_from('<IHH', actual)
   assert length + 8 == len(actual), '应用长度不符'
   assert reserved == crc == binascii.crc_hqx(actual[8:], 0), '应用 CRC 不符'
   print('应用逐字节一致，CRC 通过，SHA256:', hashlib.sha256(actual).hexdigest())
   PY
   ```

5. 确认回读一致后输入 `6` 启动。正常断电重启前**解除 BOOT 接地**，按准备页要求恢复原接线。

## 4. `user firmware invalid [-3]` 恢复

`user firmware invalid [-3]` 表示**用户应用 CRC 校验失败**。按以下步骤恢复：

1. 进入 Bootloader，核对固件校验和，并重新准备同一固件的应用文件。
2. 输入 `2`，通过 YMODEM 发送 `m1-user.serial.bin`；也可使用已与提取结果核对一致的 `phicomm_m1.usr.bin`。设置数据包间隔为 **50 ms**。
3. 等待 `Successfully!`，再执行 `2 -r`，按照上文脚本比较回读内容。
4. 回读不一致时，检查供电、共地、导线、串口占用及 USB-TTL 转换器，排除问题后重新写入。**保持 Bootloader 和参数分区，不要通过整片擦除来试错。**
5. 回读与应用文件完全一致后，输入 `6` 启动。

## 5. 如何判断应用确实运行

`Booting.......` 表示 Bootloader 正在启动应用。应用运行后：

- 用户串口使用 **115200、8N1、无流控**；**保持 921600 会看到乱码。**
- 切换至 115200 后，可在串口工具的十六进制显示模式下查看每约 5 秒发送的 12 字节传感器查询帧：

```text
23 01 00 00 00 00 00 00 00 00 00 21
```

看到周期性查询帧说明应用正在运行。

## 6. 配网

解除 BOOT 接地并恢复原接线后，连接 `M1-AP-<MAC后六位>` 热点，访问 `http://192.168.0.1/` 完成配网；随后检查网页中的传感器读数。需要 MQTT 时，再填写 Broker 并检查连接状态。

## 之后

日常操作与指示灯见 [M1 功能与操作](m1-guide)，后续升级用网页 OTA，见[固件更新](firmware-update)。
