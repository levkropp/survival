---
layout: default
title: "Chapter 28: Setting Up the ESP32"
parent: "Phase 1: The Pocket Flasher"
grand_parent: "Part 2: The ESP32 That Saves the World"
nav_order: 2
---

# Chapter 28: Setting Up the ESP32

## A Different World

In Part 1, we wrote bare-metal code for UEFI — no OS, no standard library, no memory allocator until we built one ourselves. Every byte was ours to manage.

The ESP32 is different. It runs FreeRTOS, a real-time operating system that handles task scheduling, memory allocation, and driver management. Espressif (the company behind the ESP32) provides ESP-IDF, a comprehensive SDK with SPI drivers, partition management, logging, and hundreds of other components. We get `printf`, `malloc`, `memcpy` — a full C standard library.

This might feel like cheating after 26 chapters of bare-metal programming. It's not. The ESP32's job is simple and mechanical: write bytes to an SD card. The interesting engineering is in the *what* — the GPT tables, the FAT32 filesystem, the decompression pipeline — not in bit-banging a UART. Using ESP-IDF lets us focus on the flasher logic instead of reinventing serial I/O on a microcontroller.

## Installing ESP-IDF

ESP-IDF is Espressif's official development framework. It includes the cross-compiler (xtensa-esp32-elf-gcc), the build system (CMake + Ninja), and all the ESP32 library components.

Install it to a dedicated directory:

```bash
mkdir -p ~/esp
cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32
```

The `--recursive` flag is critical — ESP-IDF has dozens of submodules. The `install.sh esp32` command installs only the ESP32 toolchain (not S2, S3, or C3 variants), saving time and disk space.

Before every build session, you must source the environment:

```bash
source ~/esp/esp-idf/export.sh
```

This adds the cross-compiler and `idf.py` (the build tool) to your PATH. If you forget this step, nothing will work — `idf.py` won't be found, and CMake won't know where the toolchain lives. It's the single most common ESP-IDF setup mistake.

## Project Structure

Create the project directory alongside the Part 1 source:

```
survival/
  esp32/
    CMakeLists.txt          Top-level build file
    partitions.csv          Flash partition layout
    sdkconfig.defaults      ESP-IDF configuration overrides
    main/
      CMakeLists.txt        Component registration
      main.c                Entry point
      display.c / .h        ILI9341 driver
      touch.c / .h          XPT2046 driver
      sdcard.c / .h         SD card I/O
      gpt.c / .h            GPT creation
      fat32.c / .h          FAT32 filesystem
      flasher.c / .h        Flash orchestrator
      payload.c / .h        Payload reader
      ui.c / .h             Touch UI
      font.c / .h           8x16 VGA font
    scripts/
      pack_payload.py       Payload packer
```

ESP-IDF projects have a specific layout. The top-level `CMakeLists.txt` declares the project. The `main/` directory is a "component" — ESP-IDF's unit of compilation. Each component has its own `CMakeLists.txt` that lists source files.

### The Top-Level CMakeLists.txt

```cmake
# Survival Workstation — ESP32 CYD SD Card Flasher
# For ESP32-2432S028R (ESP32 + ILI9341 + XPT2046 + SD slot)

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(survival-flasher)
```

Three lines of real content. `$ENV{IDF_PATH}` resolves because we sourced `export.sh`. The `include` pulls in ESP-IDF's build system, which provides everything: compiler flags, linker scripts, partition table processing, and the flash upload tool.

### The Component CMakeLists.txt

```cmake
idf_component_register(
    SRCS
        "main.c"
        "display.c"
        "touch.c"
        "sdcard.c"
        "gpt.c"
        "fat32.c"
        "flasher.c"
        "payload.c"
        "ui.c"
        "font.c"
    INCLUDE_DIRS "."
)
```

`idf_component_register` is ESP-IDF's way of declaring what goes into a component. `SRCS` lists every `.c` file. `INCLUDE_DIRS "."` means headers in the same directory are available via `#include "display.h"` — no path prefix needed.

Every time you add a new source file, it must be listed here. Forget it and the linker will complain about undefined symbols.

## The Partition Table

The ESP32 has 4 MB of flash. ESP-IDF uses a partition table to divide it into regions. The default table allocates 1 MB for the application — not enough for us, and it wastes the rest. We need a custom layout.

Create `partitions.csv`:

```
# Survival Flasher partition table (4MB flash)
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x6000,
app,      app,  factory, 0x10000,  0x160000,
payload,  data, 0x40,    0x170000, 0x290000,
```

Three partitions:

```
┌────────────────────────────────────────────────────┐
│ 0x000000 - 0x008FFF  Bootloader + partition table  │
│ 0x009000 - 0x00EFFF  NVS (24 KB)                  │
│ 0x010000 - 0x16FFFF  Application (1.375 MB)        │
│ 0x170000 - 0x3FFFFF  Payload (2.625 MB)            │
└────────────────────────────────────────────────────┘
         Total: 4 MB (0x400000)
```

**NVS** (Non-Volatile Storage) is ESP-IDF's key-value store. Various ESP-IDF components use it for calibration data, Wi-Fi credentials, and internal bookkeeping. Even though we don't use Wi-Fi, some ESP-IDF initialization code expects NVS to exist. 24 KB is the minimum.

**Application** at 1.375 MB. Our compiled firmware — the flasher code, FreeRTOS, ESP-IDF runtime, SPI drivers, and the miniz decompression library — all linked into a single binary. With size optimization (`-Os`), the firmware is well under 1 MB.

**Payload** at 2.625 MB. This is where the compressed workstation images live. The subtype `0x40` is a custom value — ESP-IDF reserves subtypes below 0x40 for its own use. We pick 0x40 as our "workstation payload" type. The flasher uses `esp_partition_find_first()` to locate this partition at runtime by type and subtype.

The offset arithmetic must be exact. Each partition starts where the previous one ends. The bootloader and partition table live at fixed addresses below 0x9000. If offsets overlap, the build system will catch it — but only at flash time, not at compile time.

## sdkconfig.defaults

ESP-IDF has hundreds of configuration options, managed through Kconfig (the same system Linux uses). Rather than running the interactive `menuconfig` tool, we specify our overrides in `sdkconfig.defaults`:

```
# Target: ESP32 (not S2/S3/C3)
CONFIG_IDF_TARGET="esp32"

# Flash: 4MB
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y

# Custom partition table
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"

# Increase app size limit (we use ~1.4MB)
CONFIG_PARTITION_TABLE_OFFSET=0x8000

# Serial output for debugging
CONFIG_ESP_CONSOLE_UART_DEFAULT=y

# Optimize for size (firmware must fit in 1.4MB)
CONFIG_COMPILER_OPTIMIZATION_SIZE=y
```

Key settings:

- **`CONFIG_IDF_TARGET="esp32"`** — We target the original ESP32, not the S2, S3, or C3 variants. The CYD board uses the ESP32-WROOM-32 module.
- **`CONFIG_ESPTOOLPY_FLASHSIZE_4MB`** — Tells the flash tool the chip has 4 MB. Wrong flash size = bricked device.
- **`CONFIG_PARTITION_TABLE_CUSTOM`** — Use our `partitions.csv` instead of the default.
- **`CONFIG_COMPILER_OPTIMIZATION_SIZE`** — Compile with `-Os` (optimize for size). The firmware must fit in 1.375 MB. Without this, it might not.

On first build, ESP-IDF generates a full `sdkconfig` from these defaults plus its built-in values. The `sdkconfig` file is large (hundreds of lines) and machine-generated — do not edit it by hand.

## Building and Flashing

With the structure in place, build:

```bash
cd esp32
source ~/esp/esp-idf/export.sh
idf.py build
```

The first build takes a few minutes — it compiles the entire ESP-IDF framework. Subsequent builds only recompile changed files and finish in seconds.

To flash the firmware to the board:

```bash
idf.py -p /dev/ttyUSB0 flash
```

The CYD board's micro-USB port provides both power and a serial connection via a CH340 USB-to-UART chip. `/dev/ttyUSB0` is the typical device name on Linux. Press and hold the BOOT button on the board while plugging in USB if the flasher can't connect — some boards need a manual boot-mode entry.

To see serial output (our `ESP_LOGI` messages):

```bash
idf.py -p /dev/ttyUSB0 monitor
```

You should see:

```
I (324) main: === Survival Workstation Flasher ===
I (334) display: Initializing ILI9341 on SPI2
I (534) display: Display ready: 320x240
I (534) touch: Initializing XPT2046 touch (bit-bang SPI)
I (534) touch: Touch ready
```

If you see this, the toolchain is working, the firmware is running, and the display and touch drivers have initialized. The display should show a splash screen.

## The Entry Point

Here is `main.c` — the starting point of the flasher:

```c
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_chip_info.h"

#include "display.h"
#include "touch.h"
#include "sdcard.h"
#include "payload.h"
#include "flasher.h"
#include "ui.h"

static const char *TAG = "main";
```

The `TAG` variable is an ESP-IDF convention. Every module defines a tag string used with `ESP_LOGI`, `ESP_LOGE`, and other logging macros. The tag appears in serial output so you can see which module produced each message.

```c
/* Chip detection — true if ESP32-S3 (has USB OTG) */
static bool has_usb_otg = false;

static void chip_detect(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);

    const char *name = "ESP32";
    if (info.model == CHIP_ESP32S3) {
        name = "ESP32-S3";
        has_usb_otg = true;
    } else if (info.model == CHIP_ESP32S2) {
        name = "ESP32-S2";
    } else if (info.model == CHIP_ESP32C3) {
        name = "ESP32-C3";
    }

    ESP_LOGI(TAG, "Chip: %s, cores: %d, flash: %s",
             name, info.cores,
             (info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");
}
```

Chip detection is forward-looking. The CYD uses a plain ESP32, but the same firmware could run on an ESP32-S3 board that has USB OTG — which could theoretically act as a USB mass storage device, presenting the workstation image as a bootable drive without even needing an SD card. We detect the chip variant and pass `has_usb_otg` to the UI so it can show a hint about future USB boot capability.

```c
void app_main(void)
{
    ESP_LOGI(TAG, "=== Survival Workstation Flasher ===");

    /* Initialize peripherals */
    display_init();
    touch_init();

    /* Read payload manifest from flash partition */
    int payload_ok = payload_init();

    /* Detect chip variant */
    chip_detect();

    /* Show splash screen */
    ui_show_splash();
    vTaskDelay(pdMS_TO_TICKS(1500));
```

`app_main` is ESP-IDF's entry point — the equivalent of `main()` in a normal C program, but running as a FreeRTOS task. The initialization sequence is deliberate: display first (so we can show errors), touch second (so we can accept input), payload third (so we know what architectures are available).

`vTaskDelay(pdMS_TO_TICKS(1500))` is FreeRTOS's sleep. The splash screen shows for 1.5 seconds — long enough to read, short enough not to annoy.

```c
    if (payload_ok != 0) {
        ui_show_error("No payload found in flash.\n"
                      "Run pack_payload.py and\n"
                      "flash the payload partition.");
        /* Spin forever — nothing else to do */
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }
```

If no payload is found, the flasher has nothing to write. This happens when you flash the firmware but forget to flash the payload partition. The error message tells you exactly what to do. The infinite loop prevents the watchdog timer from resetting the chip — FreeRTOS's idle task needs to run periodically, and `vTaskDelay` yields to it.

```c
    /* Main UI loop */
    while (1) {
        int choice = ui_show_menu(has_usb_otg);

        if (choice == UI_CHOICE_FLASH_AARCH64 || choice == UI_CHOICE_FLASH_X86_64) {
            const char *arch = (choice == UI_CHOICE_FLASH_AARCH64)
                             ? "aarch64" : "x86_64";

            /* Initialize SD card */
            if (sdcard_init() != 0) {
                ui_show_error("No SD card detected.\n"
                              "Insert a card and try again.");
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }

            /* Run the flash sequence */
            int result = flasher_run(arch);
            sdcard_deinit();

            if (result == 0) {
                ui_show_done(arch);
            } else {
                ui_show_error("Flash failed.\n"
                              "Check serial log for details.");
            }

            /* Wait for tap before returning to menu */
            ui_wait_for_tap();
        }
    }
}
```

The main loop is a state machine: show menu → wait for tap → initialize SD card → flash → show result → repeat. The SD card is initialized *after* the user makes a choice, not at startup. This lets the user insert the card at any time — they don't need to have it in the slot when the ESP32 powers on.

`sdcard_deinit()` releases the SD card after each flash cycle. This is important because the user might swap cards between flashes — reinitializing probes the card fresh each time.

The `continue` after an SD card failure returns to the top of the loop, showing the menu again. The user can insert a card and try once more.

## What We Have

A project that builds, flashes, and boots. The display shows a splash screen, the touch controller is ready, and the main loop waits for input. The serial log confirms each subsystem initialized.

The real work starts in the next chapter, where we dig into how the display and touch drivers actually work — SPI bus configuration, ILI9341 initialization, landscape orientation, and the bit-banged touch protocol.
