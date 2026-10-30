# TC1 illustrated OTA guide

The computer serves firmware over **Ethernet** and sends the trigger over **Wi-Fi**, while a router or AP supplies the OTA network. No case opening, soldering or Python installation is needed. Supports Windows, macOS and Linux.

**For TC1 A1 hardware (EMW3031 / MK3031), stock firmware 1.0.0.26, 1.0.0.24 or 1.0.0.21.** Other stock versions, TC1 A2 and the A1 air purifier are outside this procedure.

<!-- os-picker -->

## 1. Prepare equipment and downloads

Use a computer with Ethernet and Wi-Fi, an Ethernet cable, and a router or AP supporting **2.4 GHz open Wi-Fi bridged to its LAN ports**. A USB Ethernet adapter works too. Keep TC1 on stable power and disconnect loads from its outlets.

Create a `tc1-flash` folder. Download the firmware and **one trigger file matching your stock version**:

| Download | Purpose |
| --- | --- |
| [mico_ota.bin](assets/tools/tc1-no-disassembly/mico_ota.bin) | Prepared firmware for the TFTP directory; keep this filename |
| [tftp-trigger.bin (payload, 1.0.0.26)](assets/tools/tc1-no-disassembly/tftp-trigger.bin) | Trigger sent to the stock OTA endpoint; for 1.0.0.26 |
| [tftp-trigger-1.0.0.24.bin (payload, 1.0.0.24)](assets/tools/tc1-no-disassembly/tftp-trigger-1.0.0.24.bin) | Same trigger with the address shifted for 1.0.0.24 |
| [tftp-trigger-1.0.0.21.bin (payload, 1.0.0.21)](assets/tools/tc1-no-disassembly/tftp-trigger-1.0.0.21.bin) | Same trigger with the address shifted for 1.0.0.21 |

The firmware is **649,344 bytes**; each trigger file is **397 bytes**. Verify with [SHA256SUMS](assets/SHA256SUMS). See [Firmware updates](firmware-update) for later upgrades.

Prepare `curl` and a TFTP server. Download the files and tools before switching Wi-Fi networks, and keep the original filenames.

<!-- os:windows -->

Use [Tftpd64](https://pjo2.github.io/tftpd64/) for TFTP. Check curl in CMD or PowerShell:

```cmd
curl.exe --version
```

Windows commands use `curl.exe` to avoid the PowerShell alias.

<!-- /os -->

<!-- os:linux -->

Prepare `dnsmasq` for the temporary TFTP service in step 4. On Debian / Ubuntu, install `dnsmasq-base` while still online. Check curl:

```sh
curl --version
```

<!-- /os -->

<!-- os:mac -->

Use the built-in TFTP service configured in step 4. Check curl in Terminal:

```sh
curl --version
```

<!-- /os -->

## 2. Configure the AP and connect Ethernet

After receiving the trigger, TC1 leaves its provisioning hotspot and joins `PHICOMM_OTA_AP` to download firmware. Prepare that network first.

| Connection | Connects to | Purpose |
| --- | --- | --- |
| Computer Ethernet, `10.0.0.2/8` | Router / AP LAN port | Serve the firmware over TFTP |
| Computer Wi-Fi | TC1's `Phicomm_TC1_XXXX` hotspot | Read the version and send the trigger |
| TC1 after receiving the trigger | Router's `PHICOMM_OTA_AP` hotspot | Download firmware from the computer |

Configure the router or AP's **2.4 GHz** network:

| Setting | Value |
| --- | --- |
| SSID | Exactly `PHICOMM_OTA_AP`, case-sensitive, no extra spaces |
| Security / password | Open, no password |
| Hidden SSID | Off |
| Guest mode / client isolation | Off; wireless clients must reach the computer on LAN |

![Router settings: PHICOMM_OTA_AP, open security, hidden SSID and guest mode off](assets/images/tc1-ota-ap.png)

Save, then connect the computer to a **LAN port**. Internet access is unnecessary. Leave the router's management address alone: `10.0.0.2` belongs to the computer. Check for address conflicts.

The computer's adapters need **no bridge or Internet sharing**. Keep Ethernet connected to the AP and Wi-Fi connected to TC1 during flashing.

## 3. Add 10.0.0.2 to Ethernet

Use **`10.0.0.2` with mask `255.0.0.0` (/8)**, with no additional gateway or DNS. Record the original addresses, gateway, DNS and DHCP state first.

<!-- os:windows -->

### Windows

Open CMD or PowerShell as administrator:

```cmd
netsh interface ipv4 show interfaces
ipconfig /all
```

Find the Ethernet adapter connected to the router. Replace `Ethernet` below with its actual name:

```cmd
netsh interface ipv4 add address name="Ethernet" address=10.0.0.2 mask=255.0.0.0
netsh interface ipv4 show addresses name="Ethernet"
```

![Administrator terminal adding 10.0.0.2/8 to Ethernet](assets/images/tc1-ota-windows-address.png)

Confirm `10.0.0.2` appears. **This command disables DHCP on an adapter previously configured to obtain its address automatically.** Restore DHCP afterward if that was its original mode. See [Microsoft's netsh reference](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/netsh-interface).

<!-- /os -->

<!-- os:linux -->

### Linux

Find the wired adapter; replace example `enp3s0` with its actual name:

```sh
ip -br address
ip route
sudo ip address add 10.0.0.2/8 dev enp3s0
ip -4 address show dev enp3s0
```

<!-- /os -->

<!-- os:mac -->

### macOS

Distinguish Wi-Fi from Ethernet first. USB Ethernet may be `en5`, rather than `en0`; substitute your actual wired interface:

```sh
networksetup -listallhardwareports
ifconfig en5
sudo ifconfig en5 inet 10.0.0.2 netmask 255.0.0.0 alias
ifconfig en5
```

<!-- /os -->

Confirm **Ethernet actually has `10.0.0.2/8`** before continuing. Resolve missing interfaces and address conflicts first.

## 4. Start TFTP

TC1 reads `mico_ota.bin` from the computer. **Keep the payload in your terminal's working folder; it is not served through TFTP.** Keep the server running until the firmware transfer completes.

<!-- os:windows -->

### Windows: Tftpd64

1. Set **Current Directory** using **Browse** to the `tc1-flash` folder containing `mico_ota.bin`.
2. Choose **`10.0.0.2`** under **Server interfaces**, rather than the Wi-Fi address.
3. Keep **Tftp Server** open to watch transfer records. Do not enable its DHCP server.
4. Allow Tftpd64 through the firewall on the flashing network when prompted.

![Tftpd64 directory selection and Server interfaces set to 10.0.0.2](assets/images/tc1-ota-tftpd64.png)

<!-- /os -->

<!-- os:linux -->

### Linux: temporary dnsmasq TFTP

On Debian / Ubuntu, install `dnsmasq-base` before switching networks. Other distributions need the package providing `dnsmasq`. Run from the `tc1-flash` folder:

```sh
sudo apt install dnsmasq-base
tc1_tftp_dir=$(mktemp -d /tmp/tc1-tftp.XXXXXX)
chmod 755 "$tc1_tftp_dir"
install -m 644 mico_ota.bin "$tc1_tftp_dir/mico_ota.bin"
sudo dnsmasq --no-daemon --conf-file=/dev/null --port=0 \
  --listen-address=10.0.0.2 --bind-interfaces --enable-tftp \
  --tftp-root="$tc1_tftp_dir" --log-facility=-
```

Keep this terminal running and open another for curl. This temporary command provides TFTP without DNS or DHCP. If UDP 69 is already occupied, identify the existing TFTP server and use its serving directory instead of starting another instance.

<!-- /os -->

<!-- os:mac -->

### macOS: built-in TFTP

Open a terminal in `tc1-flash`. Check `/private/tftpboot` for an existing firmware file with the same name and back it up if needed, then run:

```sh
sudo mkdir -p /private/tftpboot
sudo cp mico_ota.bin /private/tftpboot/mico_ota.bin
sudo chmod 644 /private/tftpboot/mico_ota.bin
sudo launchctl load -w /System/Library/LaunchDaemons/tftp.plist
```

The built-in service serves `/private/tftpboot` without a graphical progress window. If already loaded, verify the existing service.

<!-- /os -->

### Verify file access

TFTP starts on **UDP 69**, then transfers data through a separate session port. Allow complete TFTP transfers on the flashing network, including those session ports.

Before putting TC1 in provisioning mode, download the entire file from another computer connected to `PHICOMM_OTA_AP`. If that client's curl supports `tftp`, run:

<!-- os:windows -->

```cmd
curl.exe --fail --max-time 30 -o tc1-download-check.bin tftp://10.0.0.2/mico_ota.bin
```

<!-- /os -->

<!-- os:mac linux -->

```sh
curl --fail --max-time 30 -o tc1-download-check.bin tftp://10.0.0.2/mico_ota.bin
```

<!-- /os -->

If the client uses a different OS, use its curl command or a TFTP client in **binary / octet** mode. Download from the AP-connected client and compare the size and SHA-256 with the original.

## 5. Put TC1 in provisioning mode

Power-cycle TC1 and **double-press the provisioning button within 3 seconds**. The LED should blink slowly and the open hotspot **`Phicomm_TC1_XXXX`** should appear; the last four characters vary by device.

Alternatively, hold the button until fast blinking, continue holding briefly, release, then quickly press twice. Trust the hotspot appearing. If absent, power-cycle and retry before sending any payload.

Connect the computer's **Wi-Fi** to that hotspot. Keep Ethernet and TFTP running.

<!-- os:windows -->

If Windows reports no Internet, stay connected to TC1.

<!-- /os -->

![Stock provisioning hotspot Phicomm_TC1_6D3D in the wireless list](assets/images/tc1-stock-ap.png)

Confirm Wi-Fi has a `10.10.10.x/24` address, then query the stock firmware.

<!-- os:windows -->

**Windows:**

```cmd
curl.exe --noproxy "*" --fail --max-time 5 http://10.10.10.1:8000/read-domain
```

<!-- /os -->

<!-- os:mac linux -->

```sh
curl --noproxy '*' --fail --max-time 5 http://10.10.10.1:8000/read-domain
```

<!-- /os -->

Check **`VERSION` = `1.0.0.26`, `1.0.0.24` or `1.0.0.21`** (1.0.0.21 reports `COMPILE_TIME` `Oct 29 2017 11:09:21`; the other two report `Apr 13 2018 10:09:06`). The screenshot's `COMMITID` is `d1ecc7616f55bbf781be3bd80e23bab7c03dc047` (`d22fb6bcbbb63537ce33e596278963424bf11fbb` for 1.0.0.21). Stop if the version or build identifier differs; the payload must match the detected version (use `tftp-trigger.bin` for 1.0.0.26, `tftp-trigger-1.0.0.24.bin` for 1.0.0.24 and `tftp-trigger-1.0.0.21.bin` for 1.0.0.21).

<!-- os:windows -->

![read-domain response containing stock VERSION and COMMITID](assets/images/tc1-stock-version.png)

<!-- /os -->

## 6. Send the trigger file and wait

Open a terminal in the `tc1-flash` folder containing **`tftp-trigger.bin`** (use `tftp-trigger-1.0.0.24.bin` for a 1.0.0.24 device or `tftp-trigger-1.0.0.21.bin` for a 1.0.0.21 device). Confirm TFTP is running, Ethernet still has `10.0.0.2`, and Wi-Fi is connected to TC1. Send **once**:

<!-- os:windows -->

**Windows (CMD / PowerShell):**

```cmd
curl.exe --noproxy "*" --max-time 30 --header "Content-Type: application/json" --data-binary "@tftp-trigger.bin" http://10.10.10.1:8000/OTA
```

<!-- /os -->

<!-- os:mac linux -->

```sh
curl --noproxy '*' --max-time 30 --header 'Content-Type: application/json' --data-binary '@tftp-trigger.bin' http://10.10.10.1:8000/OTA
```

<!-- /os -->

`--data-binary` preserves the payload bytes. Do not open the file and paste its contents as text, or include the prompt/backticks shown in screenshots.

<!-- os:windows -->

![Stock endpoint returns OTA text after curl sends tftp-trigger.bin](assets/images/tc1-ota-trigger.png)

<!-- /os -->

After receiving `OTA` or repeated `A` characters, wait for download and reboot. The provisioning hotspot disappearing and Wi-Fi disconnecting are expected during the network switch. Even if curl times out, observe the device and download before considering another attempt.

TC1 joins `PHICOMM_OTA_AP` and downloads `mico_ota.bin` over Ethernet through the AP. Where logs or progress are available, look for a read request and complete transfer. **The computer's Wi-Fi does not need to switch to the AP.**

Keep TC1, AP and computer powered while download, verification and reboot finish. Do not stop TFTP or disconnect Ethernet during transfer.

## 7. Confirm the community firmware starts

The new open hotspot should be **`TC1_A1-AP-<last six MAC characters in uppercase>`**. The screenshot's `TC1_A1-AP-636D3D` is an example.

![Community firmware hotspot TC1_A1-AP-636D3D](assets/images/tc1-community-ap.png)

Join it from a computer or phone, open **`http://192.168.0.1/`**, and confirm the community WebUI loads. Configure your normal Wi-Fi there. See [TC1 operation](tc1-guide) for controls and pairing.

Join the new hotspot and check the model and version in the local WebUI to complete installation.

## 8. Stop temporary services and restore networking

<!-- os:windows -->

Close Tftpd64, then remove the added address using the same interface name as step 3.

**Windows:**

```cmd
netsh interface ipv4 delete address name="Ethernet" address=10.0.0.2
```

If Ethernet originally used DHCP, restore it with the following command. For an originally static configuration, restore your recorded settings instead:

```cmd
netsh interface ipv4 set address name="Ethernet" source=dhcp
```

<!-- /os -->

<!-- os:linux -->

Press `Ctrl+C` in the temporary dnsmasq terminal, then remove the added address using the actual Ethernet interface name:

```sh
sudo ip address del 10.0.0.2/8 dev enp3s0
```

<!-- /os -->

<!-- os:mac -->

If you enabled TFTP just for this procedure, stop it. Then remove the added address using the same interface name as step 3:

```sh
sudo launchctl unload -w /System/Library/LaunchDaemons/tftp.plist
sudo ifconfig en5 inet 10.0.0.2 -alias
```

<!-- /os -->

Disable the temporary open AP or restore its original SSID and password. Remove temporary firewall exceptions and the firmware copies you added to temporary serving directories.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| curl cannot find payload | Open the terminal in `tc1-flash`; make sure the filename is not `tftp-trigger.bin.txt` and matches the device version |
| read-domain does not respond | Recheck TC1 Wi-Fi and `10.10.10.x/24`; pause VPN/proxy settings affecting local routes |
| No TFTP request | Verify open 2.4 GHz AP, exact SSID, LAN bridging, server binding and firewall |
| Transfer starts then stalls | Check session ports, computer sleep and Ethernet connection |
| Download finishes but no new hotspot | Keep power on and check file size and checksum; do not repeatedly trigger OTA |

For custom images or other network layouts, see [OTA internals and custom images](tc1-no-disassembly). For programmer-based recovery, see [wired flashing and recovery](tc1-flash).
