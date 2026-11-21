# hakios-sm6475: Xiaomi POCO M8 5G / Redmi Note 15 5G (kunzite / parrot)

One HakiOS device repository: the device (`device.conf`, `configs/`, prebuilt U-Boot loader, device tree) and its SoC family layer (`soc/`: Qualcomm SM6475 family, Snapdragon 6 Gen 3).

`tools/hakios-device.py sync` clones this repository into `devices/parrot/`.

## Hardware Specifications

| Component | Specification |
|---|---|
| Device | Xiaomi POCO M8 5G / Redmi Note 15 5G (`kunzite` / `parrot`) |
| SoC | Qualcomm Snapdragon 6 Gen 3 (SM6475 / SM6475-AB) |
| Architecture | ARMv8.2-A, 8 cores: 4x Cortex-A78 (big, 4-7) + 4x Cortex-A55 (little, 0-3) |
| Biggest Core | Core 7 (`bootcpu=7`, `cpufreq=1`) |
| GPU | Qualcomm Adreno 710 |
| Display | 1080 x 2392 OLED, 120 Hz, framebuffer at `0xb8000000` |
| Flash LED | Qualcomm SPMI v7 PMIC slave 3, register `0xd346` (`torch=1`) |
| Bootloader | U-Boot for SM6475 (`prebuilt/u-boot.bin`) |
| Device Tree | `prebuilt/device-tree.dtb` (Android kernel device tree) |

## Component Status

| Area | Status | Notes |
|---|---|---|
| **display** | works | 1080x2392 panel via U-Boot resident framebuffer at `0xb8000000`; SpringBoard UI renders |
| **cpu** | works | All 8 cores active, booted on biggest core (`cpus=8 bootcpu=7 cpufreq=1`) |
| **torch** | works | SPMI v7 PMIC slave 3 register `0xd346` flash LED (`torch=1`) |
| **buttons** | works | Power and volume buttons via PMIC (`buttons=1`) |
| **storage** | works | Resident APFS ramdisk `/dev/md0` (both 6GB slim and 8GB models) |
| **touch** | partial | Goodix GT9916 / FocalTech via SPI/I2C (`patch_touch_parrot.py`) |
| **battery** | partial | PMIC fuel gauge readings via SPMI |
| **gpu** | missing | Software rendering (llvmpipe); Adreno 710 Metal driver in progress |
| **audio** | missing | ADSP audio routing pending |
| **camera** | missing | Camera sensor bring-up pending |
| **wifi / bt / cell** | missing | Firmware blobs bring-up pending |

## Running (100% in RAM via fastboot)

From the root of the HakiOS repository:

```bash
# 1. Start the resident U-Boot loader
fastboot boot images/xnu-usb.img

# 2. Stage the root filesystem into RAM
fastboot oem ramdisk
fastboot stage images/system.apfs

# 3. Stage and run the iOS / Darwin kernel payload
fastboot stage images/run-payload.bin
fastboot oem run
```

Nothing is written to device storage; normal reboot returns to Android / HyperOS.

## Maintainer

* **Maintainer**: `unix-qv`
* **SoC Family**: `sm6475` (Qualcomm Snapdragon 6 Gen 3)
