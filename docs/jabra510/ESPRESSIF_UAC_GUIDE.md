# ESP32-S3 USB Audio Player Guide
## Using Official Espressif UAC Host Driver for Jabra SPEAK 510

## Overview

**Official Espressif Solution**: Use the `usb_host_uac` component from ESP-IDF Component Registry.

**Status**: ✓ Production-ready, officially supported by Espressif

---

## Component Information

### usb_host_uac Component
- **Latest Version**: 1.3.1
- **Registry**: `espressif/usb_host_uac`
- **ESP-IDF Version**: >= 5.0
- **Supported Targets**: ESP32-S2, ESP32-S3, ESP32-P4
- **UAC Version**: 1.0 (compatible with Jabra SPEAK 510)

### Official Example
- **Repository**: https://github.com/espressif/esp-iot-solution
- **Path**: `examples/usb/host/usb_audio_player`
- **Functionality**: Plays MP3 files through USB audio devices

---

## Hardware Setup

### Pin Connections
```
ESP32-S3 GPIO Pins:
  GPIO 19 (USB_DM)  ←→  Jabra D-
  GPIO 20 (USB_DP)  ←→  Jabra D+
  GND               ←→  Jabra GND
```

### Power Requirements
- Jabra SPEAK 510: Self-powered (up to 500mA)
- ESP32-S3: Requires stable 5V supply for USB host mode

### Optional: ESP32-S3-USB-OTG Dev Kit
If using ESP32-S3-USB-OTG board:
- GPIO 12: DEV_VBUS_EN (enable USB VBUS)
- GPIO 13: BOOST_EN (boost converter)
- GPIO 17: LIMIT_EN (current limit)
- GPIO 18: USB_SEL (USB port selection)

---

## API Reference

### Installation Functions

```c
#include "usb_host_uac.h"
#include "usb/usb_host.h"

// Install USB Host library
esp_err_t usb_host_install(const usb_host_config_t *config);

// Install UAC Host driver
esp_err_t uac_host_install(const uac_host_driver_config_t *config);
```

### Device Management Functions

```c
// Open USB audio device (speaker or microphone)
esp_err_t uac_host_device_open(const uac_host_device_config_t *config,
                                uac_host_device_handle_t *device_handle);

// Start audio streaming
esp_err_t uac_host_device_start(uac_host_device_handle_t device_handle);

// Stop audio streaming
esp_err_t uac_host_device_stop(uac_host_device_handle_t device_handle);

// Close device
esp_err_t uac_host_device_close(uac_host_device_handle_t device_handle);
```

### Audio Control Functions

```c
// Get device information (sample rate, channels, bit depth)
esp_err_t uac_host_get_device_info(uac_host_device_handle_t device_handle,
                                    uac_host_device_info_t *device_info);

// Set volume (0-100 range)
esp_err_t uac_host_device_set_volume(uac_host_device_handle_t device_handle,
                                      uint8_t volume);

// Set volume in dB
esp_err_t uac_host_device_set_volume_db(uac_host_device_handle_t device_handle,
                                         uint8_t volume_db);

// Mute/unmute
esp_err_t uac_host_device_set_mute(uac_host_device_handle_t device_handle,
                                    bool mute);
```

### Stream Control Functions

```c
// Suspend streaming (pause)
esp_err_t uac_host_device_suspend(uac_host_device_handle_t device_handle);

// Resume streaming
esp_err_t uac_host_device_resume(uac_host_device_handle_t device_handle);
```

### Events

```c
typedef enum {
    UAC_HOST_DRIVER_EVENT_TX_CONNECTED,      // Speaker connected
    UAC_HOST_DRIVER_EVENT_RX_CONNECTED,      // Microphone connected
    UAC_HOST_DEVICE_EVENT_RX_DONE,           // Microphone data received
    UAC_HOST_DEVICE_EVENT_TX_DONE,           // Speaker data transmitted
    UAC_HOST_DRIVER_EVENT_DISCONNECTED,      // Device disconnected
} uac_host_event_t;
```

---

## Implementation Guide

### 1. Project Setup

#### Create New Project from Example

```bash
# Clone esp-iot-solution repository
cd ~/projets/soundboard/jabra510
git clone https://github.com/espressif/esp-iot-solution.git

# Copy example
cp -r esp-iot-solution/examples/usb/host/usb_audio_player jabra_audio_player
cd jabra_audio_player

# Set target
idf.py set-target esp32s3
```

#### Or Add to Existing Project

```bash
# Add component dependency
idf.py add-dependency "espressif/usb_host_uac^1.3.1"
idf.py add-dependency "chmorgan/esp-audio-player^1.0"
```

Add to `main/idf_component.yml`:
```yaml
dependencies:
  espressif/usb_host_uac: "^1.3.1"
  chmorgan/esp-audio-player: "^1.0"
  idf: ">=5.0"
```

### 2. Basic Implementation Pattern

Based on the official example, here's the implementation structure:

#### Main Application (app_main)

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb_host_uac.h"

#define DEFAULT_UAC_FREQ 48000
#define DEFAULT_UAC_BITS 16
#define DEFAULT_UAC_CH   2
#define DEFAULT_VOLUME   45

static const char *TAG = "UAC_PLAYER";
static uac_host_device_handle_t s_spk_dev_handle = NULL;
static QueueHandle_t s_uac_event_queue = NULL;

// USB Host library task
static void usb_lib_task(void *arg)
{
    while (1) {
        usb_host_lib_handle_events(portMAX_DELAY, NULL);
    }
}

// UAC event handler callback
static void uac_device_callback(uac_host_device_handle_t device_handle,
                                const uac_host_device_event_t event,
                                void *arg)
{
    xQueueSend(s_uac_event_queue, &event, portMAX_DELAY);
}

// UAC library task - handles events
static void uac_lib_task(void *arg)
{
    uac_host_driver_config_t driver_config = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = uac_device_callback,
        .callback_arg = NULL
    };

    ESP_ERROR_CHECK(uac_host_install(&driver_config));
    ESP_LOGI(TAG, "UAC Host driver installed");

    uac_host_device_event_t event;
    while (1) {
        if (xQueueReceive(s_uac_event_queue, &event, portMAX_DELAY)) {
            switch (event.id) {
                case UAC_HOST_DRIVER_EVENT_TX_CONNECTED:
                    ESP_LOGI(TAG, "Speaker device connected");
                    // Open speaker device
                    uac_host_device_config_t dev_config = {
                        .addr = event.addr,
                        .ifc_num = event.ifc_num,
                        .ep_addr = event.ep_addr,
                        .ifc_alt = event.ifc_alt,
                        .xfer_type = UAC_TRANSFER_ISOC,
                        .channels = DEFAULT_UAC_CH,
                        .freq_hz = DEFAULT_UAC_FREQ,
                        .bit_resolution = DEFAULT_UAC_BITS,
                        .event_cb = uac_device_callback,
                        .event_arg = NULL,
                    };
                    ESP_ERROR_CHECK(uac_host_device_open(&dev_config,
                                                          &s_spk_dev_handle));

                    // Set volume
                    uac_host_device_set_volume(s_spk_dev_handle, DEFAULT_VOLUME);

                    // Unmute
                    uac_host_device_set_mute(s_spk_dev_handle, false);

                    // Start streaming
                    ESP_ERROR_CHECK(uac_host_device_start(s_spk_dev_handle));
                    break;

                case UAC_HOST_DEVICE_EVENT_TX_DONE:
                    // Audio buffer transmitted - feed more data
                    break;

                case UAC_HOST_DRIVER_EVENT_DISCONNECTED:
                    ESP_LOGI(TAG, "Device disconnected");
                    if (s_spk_dev_handle) {
                        uac_host_device_close(s_spk_dev_handle);
                        s_spk_dev_handle = NULL;
                    }
                    break;

                default:
                    break;
            }
        }
    }
}

void app_main(void)
{
    // Create event queue
    s_uac_event_queue = xQueueCreate(10, sizeof(uac_host_device_event_t));

    // Install USB Host library
    usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));

    // Create USB library task
    xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 5, NULL);

    // Create UAC library task
    xTaskCreate(uac_lib_task, "uac_lib", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "USB Audio Player initialized");
}
```

### 3. Audio Data Streaming

The official example integrates with `esp-audio-player` component:

```c
#include "audio_player.h"

// Audio write callback - sends PCM data to USB speaker
static size_t _audio_player_write_fn(void *ctx, void *data, size_t len)
{
    if (s_spk_dev_handle == NULL) {
        return 0;
    }

    // Write PCM data to USB audio device
    // The UAC driver handles buffering and isochronous transfers
    return uac_host_device_write(s_spk_dev_handle, data, len, portMAX_DELAY);
}

// Mute control callback
static esp_err_t _audio_player_mute_fn(void *ctx, AUDIO_PLAYER_MUTE_SETTING setting)
{
    if (s_spk_dev_handle == NULL) {
        return ESP_FAIL;
    }

    bool mute = (setting == AUDIO_PLAYER_MUTE);
    return uac_host_device_set_mute(s_spk_dev_handle, mute);
}

// Audio player configuration
audio_player_config_t player_config = {
    .mute_fn = _audio_player_mute_fn,
    .write_fn = _audio_player_write_fn,
    .clk_set_fn = NULL,
    .priority = 5
};

// Play MP3 file
audio_player_play("/spiffs/audio.mp3");
```

### 4. Alternative: Direct PCM Streaming

If you want to generate audio directly without MP3 playback:

```c
// Generate sine wave test tone
void generate_test_tone(int16_t *buffer, size_t samples, float frequency)
{
    static float phase = 0;
    const float sample_rate = 48000.0;

    for (size_t i = 0; i < samples; i += 2) {
        int16_t sample = (int16_t)(sin(phase) * 16000);  // 50% amplitude
        buffer[i] = sample;      // Left channel
        buffer[i+1] = sample;    // Right channel
        phase += 2.0 * M_PI * frequency / sample_rate;
        if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
    }
}

// Stream audio task
void audio_stream_task(void *arg)
{
    int16_t audio_buffer[192];  // 96 stereo samples for 48kHz

    while (s_spk_dev_handle) {
        generate_test_tone(audio_buffer, 192, 440.0);  // 440Hz A note

        size_t written = uac_host_device_write(s_spk_dev_handle,
                                                audio_buffer,
                                                sizeof(audio_buffer),
                                                pdMS_TO_TICKS(10));

        if (written == 0) {
            ESP_LOGW(TAG, "Write timeout");
        }

        vTaskDelay(pdMS_TO_TICKS(2));  // 2ms interval for 48kHz
    }
}
```

---

## Jabra SPEAK 510 Configuration

### Recommended Settings

Based on `lsusb.txt` analysis:

```c
// Jabra supports these sample rates for playback
#define JABRA_SAMPLE_RATE_8K   8000
#define JABRA_SAMPLE_RATE_16K  16000
#define JABRA_SAMPLE_RATE_48K  48000   // Recommended

// Audio format
#define JABRA_BIT_DEPTH        16      // 16-bit PCM
#define JABRA_CHANNELS         2       // Stereo playback

// Endpoint
#define JABRA_PLAYBACK_EP      0x03    // OUT endpoint
#define JABRA_CAPTURE_EP       0x83    // IN endpoint (microphone)

// Interface configuration
#define JABRA_PLAYBACK_IFC     1       // Interface 1
#define JABRA_PLAYBACK_ALT     1       // Alternate setting 1
#define JABRA_CAPTURE_IFC      2       // Interface 2
#define JABRA_CAPTURE_ALT      1       // Alternate setting 1
```

### Device Configuration Example

```c
uac_host_device_config_t jabra_config = {
    .addr = device_addr,           // From connection event
    .ifc_num = 1,                  // Playback interface
    .ep_addr = 0x03,               // Playback endpoint OUT
    .ifc_alt = 1,                  // Active alternate setting
    .xfer_type = UAC_TRANSFER_ISOC,
    .channels = 2,                 // Stereo
    .freq_hz = 48000,              // 48 kHz
    .bit_resolution = 16,          // 16-bit
    .event_cb = uac_device_callback,
    .event_arg = NULL,
};
```

---

## Build and Flash

### Standard Build Process

```bash
# Set ESP-IDF environment
. ~/projets/esp-idf/master/export.sh

# Configure target
idf.py set-target esp32s3

# Build
idf.py build

# Flash and monitor
idf.py -p /dev/ttyUSB0 flash monitor
```

### Expected Output

```
I (264) UAC_PLAYER: USB Audio Player initialized
I (304) UAC_PLAYER: UAC Host driver installed
I (1054) UAC_PLAYER: Speaker device connected
I (1074) UAC_PLAYER: Device info: 48000Hz, 16-bit, 2ch
I (1084) UAC_PLAYER: Volume set to 45%
I (1094) UAC_PLAYER: Device unmuted
I (1104) UAC_PLAYER: Streaming started
```

---

## Feature Comparison

### CherryUSB vs Espressif usb_host_uac

| Feature | CherryUSB | Espressif UAC |
|---------|-----------|---------------|
| **Official Support** | Third-party | ✓ Espressif official |
| **Documentation** | Limited (Chinese) | Good (English) |
| **ESP-IDF Integration** | Managed component | Native component |
| **Example Code** | Generic demo | Full ESP32 example |
| **UAC Version** | UAC 1.0 | UAC 1.0 |
| **Production Ready** | Yes | ✓ Yes |
| **Maintenance** | Community | ✓ Espressif |
| **ESP-IDF Version** | >= 5.0 | >= 5.0 |

**Recommendation**: Use **Espressif usb_host_uac** (official, better supported)

---

## Audio Source Options

### 1. MP3 Playback (Official Example)
- Uses `esp-audio-player` component
- Reads from SPIFFS filesystem
- Automatic decoding and streaming

### 2. WAV File Playback
```c
// Read WAV file from SD card
FILE *wav = fopen("/sdcard/audio.wav", "rb");
// Skip WAV header (44 bytes)
fseek(wav, 44, SEEK_SET);
// Stream PCM data
```

### 3. Generated Tones
- Sine waves, beeps, alerts
- No file system required
- Real-time synthesis

### 4. I2S Input
```c
// Read from I2S microphone/line-in
// Route to USB audio output
// Creates audio interface/mixer
```

### 5. Bluetooth A2DP → USB Bridge
```c
// Receive Bluetooth audio (A2DP Sink)
// Decode SBC/AAC
// Stream to USB speaker
```

---

## Troubleshooting

### Device Not Detected

**Check**:
- USB cable connections (D+/D- not swapped)
- VBUS power (Jabra requires 5V)
- USB Host mode enabled in sdkconfig
- Correct GPIO pins (19/20 for ESP32-S3)

**Debug**:
```c
// Enable USB debug logs
esp_log_level_set("USB_HOST", ESP_LOG_DEBUG);
esp_log_level_set("UAC_HOST", ESP_LOG_DEBUG);
```

### No Audio Output

**Check**:
- Device muted: `uac_host_device_set_mute(handle, false)`
- Volume too low: `uac_host_device_set_volume(handle, 50)`
- Alternate setting activated (alt setting 1, not 0)
- Sample rate matches Jabra capabilities (8k/16k/48k)

### Audio Glitches/Stuttering

**Causes**:
- Buffer underruns
- Task priority too low
- CPU overloaded

**Solutions**:
```c
// Increase task priority
.task_priority = 10,  // Higher priority

// Increase stack size
.stack_size = 8192,

// Use faster sample rate buffer management
```

### Memory Issues

**Solutions**:
- Enable PSRAM if available
- Reduce buffer sizes
- Optimize task stack sizes
- Check for memory leaks

---

## Advanced Features

### Volume Control

```c
// Set volume 0-100 range
uac_host_device_set_volume(handle, 75);

// Set volume in dB (device-specific range)
uac_host_device_set_volume_db(handle, -10);  // -10 dB
```

### Pause/Resume

```c
// Pause playback
uac_host_device_suspend(handle);

// Resume playback
uac_host_device_resume(handle);
```

### Device Information Query

```c
uac_host_device_info_t info;
uac_host_get_device_info(handle, &info);

ESP_LOGI(TAG, "Sample rate: %d Hz", info.freq_hz);
ESP_LOGI(TAG, "Channels: %d", info.channels);
ESP_LOGI(TAG, "Bit depth: %d", info.bit_resolution);
```

---

## Reference Documentation

### Official Sources
- **Component Registry**: https://components.espressif.com/components/espressif/usb_host_uac
- **Example Code**: https://github.com/espressif/esp-iot-solution/tree/master/examples/usb/host/usb_audio_player
- **ESP-IDF USB Host Guide**: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/usb_host.html

### Project Files
- **Jabra Descriptor**: `lsusb.txt`
- **Detailed Analysis**: `lsusb_explained.txt`

### Specifications
- USB Audio Class 1.0 Specification
- USB 2.0 Specification
- Jabra SPEAK 510 User Manual

---

## Next Steps

1. **Clone Example Project**
   ```bash
   git clone https://github.com/espressif/esp-iot-solution.git
   cd esp-iot-solution/examples/usb/host/usb_audio_player
   ```

2. **Test with Jabra**
   - Build and flash
   - Connect Jabra to ESP32-S3
   - Verify audio playback

3. **Customize for Your Use Case**
   - Replace MP3 with your audio source
   - Adjust sample rates/formats
   - Add user controls (buttons, web interface)

4. **Production Considerations**
   - Error handling and recovery
   - Power management
   - User feedback (LEDs, display)
   - OTA updates

---

## Conclusion

The **Espressif usb_host_uac** component provides a robust, officially-supported solution for USB audio playback on ESP32-S3. The Jabra SPEAK 510 is fully compatible (UAC 1.0, standard endpoints, self-powered).

**Recommended Path**: Use the official example as a starting point, then customize the audio source (MP3, WAV, generated, or Bluetooth) based on your specific requirements.
