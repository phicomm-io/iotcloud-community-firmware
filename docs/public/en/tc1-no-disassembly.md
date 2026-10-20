# Flash TC1 without disassembly

**For TC1 A1 hardware (EMW3031 / MK3031) running stock firmware 1.0.0.26.** Other stock versions, TC1 A2 and the A1 air purifier are not supported.

No case opening, no serial or SWD wiring: the device downloads a community image from a local TFTP server over Wi-Fi and switches slots by itself. No vendor cloud is involved. Read [Before flashing](flash-prep) and [Firmware updates](firmware-update) first.

Flashing erases the inactive application slot and reboots the device. The retained stock slot is **not** a guaranteed automatic rollback. Keep power connected and connect no load during the transfer.

## What you need

| Role | Requirement |
| --- | --- |
| Provisioning client | Joins the TC1 provisioning AP, gets `10.10.10.x/24` and can reach `10.10.10.1:8000` |
| OTA AP and file server | 2.4 GHz open AP `PHICOMM_OTA_AP`; the device can reach TFTP at `10.0.0.2/8` and fetch `mico_ota.bin` |

Any OS will do. One computer or phone can join the TC1 AP while another computer, travel router or wireless AP carries the OTA network. TFTP can run on the AP itself or on a computer behind a bridge.

If one device carries both roles it must run a Wi-Fi client and an AP at the same time: create the OTA AP first, then join the TC1 AP. Not every wireless device supports that combination; two separate devices are easier.

## Build the image

The update file is the raw **MRVL application-slot image plus its 16-byte binary MD5**, not a 32-character text digest. The community image is 649328 bytes; the steps below produce a 649344-byte `mico_ota.bin`:

```sh
python3 - <<'PY'
import hashlib
image = open('tc1_community.image.bin', 'rb').read()
assert image.startswith(b'MRVL'), 'raw MRVL application-slot image required'
assert len(image) + 16 <= 0xA9000, 'image must fit the 692224 B slot'
open('mico_ota.bin', 'wb').write(image + hashlib.md5(image).digest())
print(len(image), hashlib.md5(image).hexdigest())
PY
```

The image MD5 should be `137bbe25d40f0a45b2b3129f035ea4c3`. Do not use a full-flash dump, a complete `.all.bin` or a `PHOTA001` web package as input; these checks only cover the MRVL marker, size and MD5, not hardware suitability.

## Enter provisioning mode

Either method works. The success state is the same: slow blinking LED, open AP `Phicomm_TC1_XXXX` (`XXXX` = last four MAC characters), address `10.10.10.1/24` and the `:8000` service.

- **Method 1:** power-cycle, then **double-press the provisioning button within 3 seconds** of power-up. The LED turns to ~500 ms slow blink and the AP appears.
- **Method 2:** long-press the provisioning button, wait for fast blink, keep holding for a while, release, then quickly short-press twice. The AP appearing confirms provisioning mode. Fast-blink timing can differ between stock versions, so trust the AP rather than the light.

If the AP does not appear, power-cycle and retry method 1. Then re-check hardware and stock version; do not spam the button.

Join `Phicomm_TC1_XXXX` and check the identity first:

```sh
curl -s -m 5 http://10.10.10.1:8000/read-domain
```

`VERSION` must be `1.0.0.26` with `COMPILE_TIME` `Apr 13 2018 10:09:06`. Do not continue on a different build.

## Create the OTA AP and TFTP service

1. Create a **2.4 GHz** AP named exactly **`PHICOMM_OTA_AP`**, open, no password, no client isolation.
2. Give the TFTP-serving interface the static address **`10.0.0.2`** with mask **`255.0.0.0` (/8)**. When the server sits outside the AP, bridge the AP into the server's layer-2 network.
3. Serve **`mico_ota.bin`** over TFTP: octet mode, 512-byte blocks, no option negotiation.
4. Allow the device to reach the server for the initial UDP 69 request **and** the follow-up data session on the server's own port.
5. Verify from another client that the whole file downloads and the digest matches. A loopback download only proves the file service works.

Without an existing daemon, this read-only Python service serves exactly that file:

```sh
python3 - <<'PY'
import socket, struct
data = open('mico_ota.bin', 'rb').read()
srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
srv.bind(('10.0.0.2', 69))
print('listening on 10.0.0.2:69,', len(data), 'B')
while True:
    req, peer = srv.recvfrom(2048)
    if req[2:].split(b'\0')[:2] != [b'mico_ota.bin', b'octet']:
        srv.sendto(struct.pack('!HH', 5, 4) + b'only mico_ota.bin octet\0', peer)
        continue
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as stream:
        stream.bind(('10.0.0.2', 0))
        stream.settimeout(3)
        for offset in range(0, len(data) + 1, 512):
            block = offset // 512 + 1
            packet = struct.pack('!HH', 3, block) + data[offset:offset + 512]
            for _ in range(5):
                stream.sendto(packet, peer)
                try:
                    ack, sender = stream.recvfrom(516)
                except socket.timeout:
                    continue
                if sender == peer and ack == struct.pack('!HH', 4, block):
                    break
print('done')
PY
```

Port 69 needs elevated privileges on Linux and macOS; on Windows allow UDP through the firewall for the OTA adapter.

## Trigger the flash

With both network roles ready, generate the community-provided trigger file and send it once from the provisioning client:

```sh
python3 - <<'PY'
body = b'A' * 392 + (0x1F073D9F).to_bytes(4, 'little') + b'\0'
open('tftp-trigger.bin', 'wb').write(body)
print(len(body))
PY
curl --max-time 30 --header 'Content-Type: application/json' \
  --data-binary @tftp-trigger.bin \
  http://10.10.10.1:8000/OTA
```

`OTA AAAA...` only means the request was accepted — **not** that flashing succeeded. If the connection drops, watch the device and do not retrigger immediately.

## Watch the transfer and first boot

After leaving provisioning mode the device joins `PHICOMM_OTA_AP` with `10.<last three MAC bytes as decimals>` and mask `/8`. This differs per device; do not copy the example.

Typical order: join the OTA AP → erase the inactive slot → download `mico_ota.bin` from `10.0.0.2` → verify MD5 → switch slots and reboot. The TFTP log should show the complete transfer.

After a successful first boot the open AP **`TC1_A1-AP-<last six MAC chars>`** appears at **`192.168.0.1`**. Join it and open the device's local web UI to finish setup.

## Acceptance and cleanup

Record four things separately: stock identity, complete TFTP transfer, new firmware AP, and web UI reachability. HTTP 200, a device drop-off or one TFTP transfer alone do not prove success.

Then shut down the temporary OTA AP and TFTP service and restore the interface addresses and firewall rules you changed. On OpenWrt / ImmortalWrt, adapt the temporary interface and dnsmasq TFTP setup to the goals above, then remove those settings and restore your original network.

## Common problems

| Symptom | Likely cause |
| --- | --- |
| No `Phicomm_TC1_XXXX` | Wrong button rhythm, or not A1 hardware / not 1.0.0.26 |
| `/OTA` returns 404 or hangs | Not in provisioning mode, or client outside `10.10.10.x/24` |
| Device never joins the OTA AP | Not 2.4 GHz, wrong SSID, or client isolation enabled |
| TFTP stalls after the first block | Firewall allows only UDP 69, not the transfer session |
| TFTP completes but no slot switch | Image MD5 mismatch, or image larger than 692224 B |

This page only covers the devices and version listed above. Other stock versions are not supported.
