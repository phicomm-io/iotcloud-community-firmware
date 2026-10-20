# Flash TC1 without disassembly

**For TC1 A1 hardware (EMW3031 / MK3031) running stock firmware 1.0.0.26.** Other stock versions, TC1 A2 and the A1 air purifier are not supported.

Install community firmware over Wi-Fi without opening the case, soldering, serial or SWD wiring, or the vendor cloud.

## How it works

In provisioning mode, the stock firmware exposes a local configuration service. Its OTA endpoint has a stack-overflow vulnerability. A specially crafted request (payload) from the computer redirects execution into the firmware's own **factory OTA update process**.

TC1 then joins the open Wi-Fi network `PHICOMM_OTA_AP` and downloads `mico_ota.bin` from the TFTP server at `10.0.0.2`. It verifies the file, switches firmware slots and reboots into community firmware.

There are two connections: **the computer first joins the TC1 provisioning AP to send the trigger; TC1 then joins the router's AP to download the firmware.** Prepare the OTA AP and file server before sending the trigger. The connections do not need to share a Wi-Fi adapter.

## Prepare for flashing

### Equipment and tools

- A **TC1 A1 hardware power strip** running stock firmware **1.0.0.26**.
- A computer with **Ethernet and Wi-Fi adapters**, and a router that supports **2.4 GHz open Wi-Fi**.
- An Ethernet cable, Python 3, `curl` and **tftpd or another TFTP server** on the computer. The Python TFTP service provided on this page is another option.
- The community application-slot image `tc1_community.image.bin` for this hardware.

Other equivalent topologies work too. You need a client that can join the TC1 provisioning AP to send the trigger, and a file server that TC1 can reach through the OTA AP afterward.

Review [power and wiring requirements](flash-prep) and [firmware package selection](firmware-update). Flashing erases the inactive application slot and reboots the device. The retained stock slot is **not** a guaranteed automatic rollback.

### Prepare the update file

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

## Flashing procedure

### Before you start

1. Put `mico_ota.bin` in the computer's TFTP serving directory and open a terminal and the TFTP log window.
2. Keep TC1 on stable power with no loads connected to its outlets.
3. Set up and verify the OTA AP and TFTP service before putting TC1 into provisioning mode and sending the trigger. Keep Ethernet, the router and TFTP running throughout flashing.

### 1. Create the OTA AP and TFTP service

#### Recommended: computer with Ethernet and Wi-Fi, plus a router

The computer serves firmware over Ethernet and connects to the TC1 provisioning AP over Wi-Fi. The router provides the OTA AP for downloading the firmware.

```text
Computer Ethernet (10.0.0.2/8, TFTP server) ──cable── router LAN port
Computer Wi-Fi ──Wi-Fi── TC1 provisioning AP Phicomm_TC1_XXXX
TC1 after entering factory OTA mode ──Wi-Fi── router PHICOMM_OTA_AP
```

1. Set the router's **2.4 GHz Wi-Fi** name to exactly **`PHICOMM_OTA_AP`**, with **no password** and no client isolation. Bridge this Wi-Fi network to the LAN ports.
2. Connect the computer's Ethernet adapter to a router **LAN port**. Set its address to **`10.0.0.2`** with mask **`255.0.0.0` (/8)**; no gateway or DNS is needed. Check that the router and other devices do not already use `10.0.0.2`.
3. Start **tftpd or another TFTP server** on the computer. Serve `mico_ota.bin` from its directory and bind it to the Ethernet address `10.0.0.2`. Allow octet reads with 512-byte blocks and no option negotiation.
4. Allow TFTP traffic through the firewall: the initial request uses **UDP 69**, and the data session uses another server port that must also be allowed. Binding port 69 usually needs additional privileges on Linux/macOS; on Windows, allow UDP on the OTA adapter.
5. Use another client connected to the router's Wi-Fi to download the whole `mico_ota.bin` and check its size and MD5. A download on the server itself only checks the file service, not Wi-Fi connectivity. Keep TFTP and its log window open after this check.

The computer's adapters **need no Internet sharing or bridge between them**. Ethernet serves `10.0.0.2`, while Wi-Fi reaches TC1 at `10.10.10.1`. After joining TC1, the computer's Wi-Fi adapter does not need to switch to the router's AP.

#### Other equivalent topologies

Any OS and device combination can be used. A computer or phone can join the TC1 provisioning AP while another computer, travel router or wireless AP provides the OTA network. TFTP can run on the AP itself or on an Ethernet computer bridged to the AP.

Every setup must provide an **open 2.4 GHz AP named `PHICOMM_OTA_AP` and a TFTP server at `10.0.0.2/8` reachable through that AP**, serving `mico_ota.bin`. If one Wi-Fi adapter both joins the TC1 AP and provides the OTA AP, its hardware and driver must support simultaneous client and AP operation.

#### Optional: serve the file with Python

Without an existing TFTP application, this read-only Python service serves exactly that file:

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

### 2. Put TC1 into provisioning mode

Either method works. The success state is the same: slow blinking LED, open AP `Phicomm_TC1_XXXX` (`XXXX` = last four MAC characters), address `10.10.10.1/24` and the `:8000` service.

- **Method 1:** power-cycle, then **double-press the provisioning button within 3 seconds** of power-up. The LED turns to ~500 ms slow blink and the AP appears.
- **Method 2:** long-press the provisioning button, wait for fast blink, keep holding for a while, release, then quickly short-press twice. The AP appearing confirms provisioning mode. Fast-blink timing can differ between stock versions, so trust the AP rather than the light.

If the AP does not appear, power-cycle and retry method 1. Then re-check hardware and stock version; do not spam the button.

Join `Phicomm_TC1_XXXX` using the computer's Wi-Fi adapter and check that it has a `10.10.10.x/24` address. Keep Ethernet and TFTP running. In a terminal on the computer, read back the stock firmware identity:

```sh
curl -s -m 5 http://10.10.10.1:8000/read-domain
```

`VERSION` must be `1.0.0.26` with `COMPILE_TIME` `Apr 13 2018 10:09:06`. Do not continue on a different build.

### 3. Send the trigger request

Keep the computer's Wi-Fi adapter connected to the TC1 provisioning AP and run these commands in a terminal. Python creates the stack-overflow trigger file; `curl` sends it to the device. Send it once, then watch the device and TFTP logs:

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

### 4. Wait for the download and first boot

After entering factory OTA mode, TC1 leaves its provisioning AP and joins the router's `PHICOMM_OTA_AP`. The computer losing its Wi-Fi connection is expected. Keep Ethernet and TFTP running.

Watch the computer's **TFTP logs** for a request and download of `mico_ota.bin`. The **computer sends the file and TC1 downloads it**. TFTP tools typically show a read request (RRQ) or a file-sent record.

On the OTA network, TC1 uses `10.<last three MAC bytes as decimals>` with mask `/8`; its address differs per device. The usual sequence is: join the OTA AP → erase the inactive slot → download firmware → verify MD5 → switch slots and reboot. Keep power connected until the whole process completes.

After a successful first boot, the open AP **`TC1_A1-AP-<last six MAC chars in uppercase>`** appears. Join it with a computer or phone and open **`http://192.168.0.1`** in a browser to continue setup in the community firmware's local web UI.

### 5. Confirm success and restore your network

Check the stock identity, complete TFTP transfer, new firmware AP and local web UI separately. HTTP 200, a device disconnect or a TFTP transfer alone does not prove success.

Once the new firmware works, shut down the temporary OTA AP and TFTP service and restore the interface addresses and firewall rules changed for flashing. If you created temporary interfaces and dnsmasq TFTP settings on OpenWrt / ImmortalWrt, remove those settings and restore the original network.

## Common problems

| Symptom | Likely cause |
| --- | --- |
| No `Phicomm_TC1_XXXX` | Wrong button rhythm, or not A1 hardware / not 1.0.0.26 |
| `/OTA` returns 404 or hangs | Not in provisioning mode, or client outside `10.10.10.x/24` |
| Device never joins the OTA AP | Not 2.4 GHz, wrong SSID, or client isolation enabled |
| TFTP stalls after the first block | Firewall allows only UDP 69, not the transfer session |
| TFTP completes but no slot switch | Image MD5 mismatch, or image larger than 692224 B |

This page only covers the devices and version listed above. Other stock versions are not supported.
