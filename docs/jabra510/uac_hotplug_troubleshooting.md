# UAC Speaker Hotplug Troubleshooting

## Problem Description

The Jabra SPEAK 510 USB speaker connected to ESP32-S3 has unreliable initialization depending on power-up sequence.

**Symptoms:**
- Speaker sometimes initializes but produces no sound
- Works when speaker powers on fresh (certain sequences)
- Fails when speaker is already powered on when ESP32's USB PHY becomes active

## Test Sequences Performed

| Sequence | Description | Result |
|----------|-------------|--------|
| seq1 | Speaker powered first, MCU reset held, then release | SUCCESS |
| seq2 | MCU first, then plug speaker | FAIL |
| seq3 | Simultaneous power-up | SUCCESS |
| seq4 | MCU reset with speaker already on | FAIL |
| seq5 | MCU reset + 5sec delay with speaker on | FAIL |
| seq6 | Turn speaker on after MCU ready | FAIL |
| alternate | Hold reset, plug speaker, wait, release reset | SUCCESS |

**Pattern:** Success correlates with fresh speaker power-up or speaker connecting during MCU boot hold.

## Diagnostic Logging Added

Added TX transfer statistics to UAC driver (`uac_host.c`):

```c
// Debug variables (around line 103)
static uint32_t s_tx_submit_count = 0;
static uint32_t s_tx_complete_count = 0;
static uint32_t s_tx_error_count = 0;
static uint32_t s_tx_total_bytes = 0;
```

**Locations modified:**
- `stream_tx_xfer_submit()` - logs submit count and bytes
- `stream_tx_xfer_done()` - logs completion and errors
- `uac_host_device_open()` - resets stats on new device
- Added `uac_host_debug_dump_stats()` function

## Key Finding

**USB transfers are identical in both success and failure cases:**

| Metric | Failed init | Successful init |
|--------|-------------|-----------------|
| TX submit count | 10 | 10 |
| TX done count | 10 | 10 |
| TX errors | 0 | 0 |
| Bytes per transfer | 576 | 576 |
| actual_num_bytes | 576 | 576 |

**Conclusion:** The ESP32 successfully sends audio data to the speaker in both cases. The speaker acknowledges receipt. The problem is **internal to the Jabra speaker** - it accepts data but doesn't route it to its audio codec in the failed case.

## Root Cause Hypothesis

The Jabra SPEAK 510 has internal power-on state machine behavior:
- When it boots fresh and detects USB host enumeration as part of its startup, it properly initializes its audio path
- When already powered and "settled", USB enumeration and SET_INTERFACE commands are processed at USB protocol level, but the internal audio codec doesn't reconfigure

## Workarounds Attempted

### 1. SET_INTERFACE Cycling (FAILED)

Added to `uac_host_device_start()` in `uac_host.c` (around line 2368):

```c
// Workaround: SET_INTERFACE cycling to reset speaker audio path
{
    usb_setup_packet_t usb_request;
    uac_cs_request_t uac_request = {0};

    // Set interface to alternate 0 (zero bandwidth / idle)
    USB_SETUP_PACKET_INIT_SET_INTERFACE(&usb_request, iface->dev_info.iface_num, 0);
    memcpy(&uac_request, &usb_request, sizeof(usb_setup_packet_t));
    ret = uac_cs_request_set(iface->parent, &uac_request);

    vTaskDelay(pdMS_TO_TICKS(50));
    // Normal flow will SET_INTERFACE back to active alternate
}
```

**Result:** Does not fix the issue.

## Remaining Workarounds to Try

### 2. USB Port Reset
Force USB bus reset to make the speaker think it's being freshly connected.

```c
// In main.c or uac.c after device detection
usb_host_device_reset(device_handle);
```

### 3. Silent Audio Priming
Send a burst of silent audio before actual playback to "wake up" the audio path.

```c
// Before first real playback
uint8_t silence[576] = {0};
for (int i = 0; i < 10; i++) {
    uac_host_device_write(handle, silence, sizeof(silence), 100);
    vTaskDelay(pdMS_TO_TICKS(10));
}
```

### 4. Power Cycle Speaker via GPIO (Hardware)
If a GPIO controls speaker power, cycle it during init.

### 5. Multiple SET_INTERFACE with Longer Delays
Try cycling multiple times with longer delays between transitions.

### 6. USB Device Re-enumeration
Force complete USB re-enumeration by closing and reopening the device.

## Files Modified

**UAC Driver (local esp-usb fork):**
- `/home/vintz/projets/esp-idf/esp-usb/host/class/uac/usb_host_uac/uac_host.c`
  - Added debug statistics variables
  - Added logging in TX submit/done functions
  - Added stats reset in device open
  - Added SET_INTERFACE cycling workaround (ineffective)
  - Added `uac_host_debug_dump_stats()` function

- `/home/vintz/projets/esp-idf/esp-usb/host/class/uac/usb_host_uac/include/usb/uac_host.h`
  - Added `uac_host_debug_dump_stats()` declaration

## Log Files

Located in `/home/vintz/projets/soundboard/`:
- `seq1.log` through `seq6.log`
- `alternate seq.log`
- `reset_with_speaker_plugged_in.log`

## Next Steps

1. Try USB port reset workaround
2. Try silent audio priming
3. Consider hardware-level power cycling
4. Investigate if Jabra has vendor-specific USB commands for audio path reset
5. Check if issue exists with other UAC speakers (to determine if Jabra-specific)

## References

- ESP-IDF USB Host documentation
- USB Audio Class 1.0 specification
- Jabra SPEAK 510: VID 0x0b0e, PID 0x0422
