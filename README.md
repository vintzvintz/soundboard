# ESP32-S3 I2S Audio Soundboard

A soundboard application for ESP32-S3 that plays WAV audio files through an external I2S DAC/amplifier. Features a 4x3 matrix keypad for triggering sounds, rotary encoder for volume control and page switching, OLED display, and SD card storage.

**Current Status:** Build complete - Hardware tested and working

## Key Features

* **I2S Audio Output**: High-quality audio via external DAC/amplifier (MAX98357A, PCM5102, etc.)
* **Matrix Keypad**: 4x3 button matrix (12 buttons) with configurable actions per button
* **Rotary Encoder**: Volume control with push-button for page switching
* **PSRAM Audio Cache**: LRU cache with background preloading for instant playback
* **Multi-Page Support**: Organize sounds into pages (e.g., "default", "music", "fx")
* **USB MSC Update**: Interactive menu for USB flash drive sync (full update, incremental, SD card clear)
* **SD Card Storage**: Large audio file capacity with automatic SPIFFS fallback
* **OLED Display**: Layout-based visual feedback (idle, playing, page select, MSC menus)
* **4 Action Types**: play, play_cut, play_lock, stop (volume via rotary encoder only)
* **NVS Volume Persistence**: Volume setting survives power cycles
* **Per-Module Status System**: Hierarchical status reporting (compact/normal/verbose) for all modules

## Audio Output

This soundboard uses **I2S** to output audio to an external DAC or amplifier module:

* **Supported DAC modules**: MAX98357A, PCM5102, UDA1334A, or any I2S-compatible DAC
* **Audio format**: WAV PCM files (16-bit recommended)
* **Sample rates**: Auto-detected from WAV files (typically 44.1kHz or 48kHz)
* **Channels**: Mono or stereo (auto-detected)

**I2S GPIO connections:**
| Signal | GPIO | Description |
|--------|------|-------------|
| LRC | GPIO12 | Word Select (Left/Right Clock) |
| BCLK | GPIO13 | Bit Clock |
| DIN | GPIO14 | Data Out to DAC |
| SD | GPIO47 | Amplifier Shutdown (active low, optional) |

## Hardware Requirements

### Development Board

* ESP32-S3 board with PSRAM (recommended for audio caching)
* USB Host capability for MSC sync feature

### Components

* **I2S DAC/Amplifier**: MAX98357A, PCM5102, or similar I2S module
* **Matrix Keypad**: 4x3 button matrix (standard membrane or mechanical)
* **Rotary Encoder**: KY-040 or similar with quadrature output and push switch
* **OLED Display**: SSD1306 128x64 I2C (optional but recommended)
* **SD Card Module**: SPI interface (FAT32 formatted)
* **Speaker**: Connected to DAC/amplifier output

### GPIO Connections

**Default GPIO Assignments** (configurable via `idf.py menuconfig`):

| Component | Signal | Default GPIO |
|-----------|--------|--------------|
| I2S LRC | Word Select | GPIO12 |
| I2S BCLK | Bit Clock | GPIO13 |
| I2S DIN | Data Out | GPIO14 |
| I2S SD | Amp Shutdown | GPIO47 |
| Matrix Row 0 | Output | GPIO10 |
| Matrix Row 1 | Output | GPIO7 |
| Matrix Row 2 | Output | GPIO6 |
| Matrix Row 3 | Output | GPIO9 |
| Matrix Col 0 | Input (pull-up) | GPIO11 |
| Matrix Col 1 | Input (pull-up) | GPIO4 |
| Matrix Col 2 | Input (pull-up) | GPIO5 |
| SD Card MOSI | SPI | GPIO41 |
| SD Card MISO | SPI | GPIO39 |
| SD Card CLK | SPI | GPIO40 |
| SD Card CS | SPI | GPIO42 |
| Display SDA | I2C | GPIO8 |
| Display SCL | I2C | GPIO18 |
| Encoder CLK | Input | GPIO15 |
| Encoder DT | Input | GPIO16 |
| Encoder SW | Input | GPIO17 |

See [docs/GPIO_ALLOCATION.txt](docs/GPIO_ALLOCATION.txt) for detailed pin usage.

## Build and Flash

### Prerequisites

1. **ESP-IDF 6.0** installed and configured
2. **Local esp-usb repository** for MSC host driver:
   ```bash
   git clone https://github.com/espressif/esp-usb.git $HOME/projets/esp-idf/esp-usb
   ```
3. **IDF Component Manager** version 1.1.4 or later:
   ```bash
   pip install "idf-component-manager~=1.1.4"
   ```

### Build Steps

1. Set up ESP-IDF environment:
   ```bash
   . $IDF_PATH/export.sh
   ```

2. Set target chip:
   ```bash
   idf.py set-target esp32s3
   ```

3. Configure GPIO pins (optional):
   ```bash
   idf.py menuconfig
   ```
   Navigate to **Soundboard Application** to configure hardware pins.

4. Build, flash, and monitor:
   ```bash
   idf.py -p PORT flash monitor
   ```

(To exit the serial monitor, type `Ctrl-]`.)

## Configuration

### Button Mappings (mappings.csv)

Create a `mappings.csv` file on your SD card to map buttons to sounds:

```csv
# Format: page_id,button,event,action[,file]

# Page "default" - Sound Effects
default,1,press,play,laser.wav
default,2,press,play_cut,explosion.wav
default,3,press,play_lock,siren.wav
default,4,press,play,alarm.wav
default,12,press,stop

# Page "music" - Music tracks
music,1,press,play_lock,song1.wav
music,2,press,play_lock,song2.wav
```

**Button Layout (4x3 matrix):**
```
 1  2  3    (row 0)
 4  5  6    (row 1)
 7  8  9    (row 2)
10 11 12    (row 3)
```

**Supported Actions:**
| Action | Parameters | Description |
|--------|------------|-------------|
| `play` | file | Play once until end of file |
| `play_cut` | file | Play once, stop immediately on button release |
| `play_lock` | file | Press=play with stop on release, long press=lock playback, press again=stop |
| `stop` | - | Stop current playback |

Volume is controlled exclusively via the rotary encoder (not mappable to buttons).

**Events:** `press`, `long_press`, `release`

See [mappings.csv.example](mappings.csv.example) for more examples.

### Encoder Controls

* **Rotation**: Adjusts volume (default) or changes page
* **Short press**: Toggle between VOLUME and PAGE modes
* **In VOLUME mode**: Volume displayed in large font on OLED
* **In PAGE mode**: Dedicated page select screen with inverse video header and large page name; rotation cycles through available pages

### USB MSC Update

To update sounds without removing the SD card:

1. Prepare a USB flash drive with:
   - `mappings.csv` in a `/soundboard/` directory
   - WAV files referenced in `soundboard/mappings.csv`
2. Plug the USB drive into the ESP32-S3 USB port
3. An interactive menu appears on the OLED display with three options:
   - **Full update**: Copy all WAV files and mappings from USB to SD card
   - **Incremental update**: Validate and overwrite mappings.csv, copy only new or changed WAV files (skips files with same name and size)
   - **Clear SD card**: Erase all files on the SD card (requires confirmation)
4. Navigate with the rotary encoder, confirm with encoder press
5. SD card clear has a safety gate: requires pressing a bottom-row button (10-12) to confirm
6. Unplug the USB drive when done — the device reboots with the new configuration

## Architecture

### Audio Pipeline

```
Button Press → Mapper → Player Task → Audio Provider → I2S Driver → DAC → Speaker
                                           ↓
                                    [PSRAM Cache]
                                           ↓
                                    [WAV File on SD]
```

### Task Structure

| Task | Priority | Core | Function |
|------|----------|------|----------|
| USB Host | 5 | 0 | USB library events |
| MSC Driver | 5 | 0 | Mass storage class events |
| MSC FSM | 2 | 0 | Interactive update menu & file operations |
| Player | 2 | 1 | Audio playback (I2S) |
| Input Scanner | 3 | 1 | Matrix + encoder polling |

### Module Organization

```
main/
├── main.c              # Entry point, phased initialization
├── soundboard.h        # Shared types (app mode, status output)
├── app_state.h         # Private app state struct
├── core/               # Platform modules
│   ├── input_scanner   # Unified input handling
│   ├── console         # UART CLI with status command
│   ├── display         # OLED driver (layout-based)
│   └── sd_card         # SD card API + erase + status
├── player/             # Audio engine
│   ├── player          # I2S playback task
│   ├── provider        # WAV decoder + cache
│   ├── mapper          # Button-to-action mapping
│   └── persistent_volume
└── usb/                # USB host
    └── msc             # Interactive MSC update (FSM task)
```

## Console Commands

Connect via UART (115200 baud) to access:

| Command | Description |
|---------|-------------|
| `status <module\|all\|help> [compact\|normal\|verbose]` | Per-module status reporting |
| `play <file>` | Play audio file |
| `stop` | Stop playback |
| `volume [0-31]` | Get or set volume level |
| `system_status` | Show system info and heap usage |
| `application_status` | Show application state |
| `cat_mappings` | Display loaded button mappings |
| `ls <path>` | List files on SD card |
| `erase_sdcard` | Wipe all files from SD card |

**Status modules:** `app`, `mapper`, `sdcard`, `msc`, `input`, `display`, `volume`, `player`, `all`

## Troubleshooting

**No audio output:**
* Check I2S wiring (LRC, BCLK, DIN connections to DAC)
* Verify DAC module is powered (usually 3.3V or 5V)
* Check SD (shutdown) pin if using amplifier module
* Test with console: `play /sdcard/test.wav`
* Verify WAV file format (PCM, 16-bit)

**Buttons not responding:**
* Verify GPIO configuration in menuconfig
* Check matrix wiring (rows are outputs, columns are inputs with pull-ups)
* Use `status input verbose` to verify input scanner is running and check GPIO assignments

**SD card not detected:**
* Check FAT32 formatting
* Verify SPI GPIO pins match your wiring
* Use `status sdcard verbose` to check card info and free space
* Test with `ls /sdcard` in console

**USB sync not working:**
* Ensure USB flash drive is FAT32 formatted
* Check that mappings.csv is valid (no syntax errors)
* Verify WAV files referenced in mappings exist on the drive

**Volume not persisting:**
* NVS partition may need initialization
* Check console for NVS errors on boot

## Project Files

| File | Description |
|------|-------------|
| [mappings.csv.example](mappings.csv.example) | Example button mappings |
| [docs/GPIO_ALLOCATION.txt](docs/GPIO_ALLOCATION.txt) | Pin assignments |
| [partitions.csv](partitions.csv) | Flash partition table |
| [CLAUDE.md](CLAUDE.md) | Developer documentation |
| [sons/generate_mapping.py](sons/generate_mapping.py) | Generate mappings CSV from existing config + new WAV files |
| [sons/generate_sheet.py](sons/generate_sheet.py) | Generate HTML mapping sheets (print, desktop, mobile) |
| [sons/normalize.sh](sons/normalize.sh) | Normalize audio files to WAV (mono 48kHz) via ffmpeg |

## References

* [ESP-IDF Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/)
* [ESP-IDF I2S Driver](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/i2s.html)
* [esp-usb Repository](https://github.com/espressif/esp-usb)
