# Jabra SPEAK 510 Descriptor Parsing Fix

## Problem

The Jabra SPEAK 510 USB speakerphone violates the USB Audio Class 1.0 specification by placing descriptors in non-standard order:

### Standard-Compliant Order (UAC 1.0 Spec)
```
Interface Descriptor (Alternate Setting 1)
├─ AudioStreaming Interface Descriptor (AS_GENERAL)
├─ AudioStreaming Interface Descriptor (FORMAT_TYPE)
├─ Endpoint Descriptor ← Standard endpoint (wMaxPacketSize)
└─ AudioStreaming Endpoint Descriptor (CS_ENDPOINT) ← Class-specific
```

### Jabra SPEAK 510 Actual Order
```
Interface Descriptor (Alternate Setting 1)
├─ AudioStreaming Interface Descriptor (AS_GENERAL)
├─ AudioStreaming Interface Descriptor (FORMAT_TYPE)
├─ AudioStreaming Endpoint Descriptor (CS_ENDPOINT) ← Class-specific comes FIRST
└─ Endpoint Descriptor ← Standard endpoint comes AFTER
```

## Symptom

The original UAC driver code stopped parsing after finding the CS_ENDPOINT descriptor, assuming all necessary information had been collected. This caused:

- `ep_mps` (endpoint max packet size) remained **0**
- `ep_addr` (endpoint address) remained unset
- Stream direction (TX/RX) could not be determined
- **Assertion failure**: `packet_size <= ep_mps` fails when ep_mps is 0

### Error Log
```
D (1467) uac-host: Endpoint MPS: 0
D (1470) uac-host: Final packet_size: 192 (ep_mps: 0)
assert failed: uac_host_device_start uac_host.c:2324 (iface->packet_size <= iface->iface_alt[iface->cur_alt].ep_mps)
```

## Solution

Modified the descriptor parsing logic in `uac_host_interface_add()` to handle both compliant and non-compliant devices.

### Code Change ([uac_host.c:814-831](components/espressif__usb_host_uac/uac_host.c#L814))

**Before:**
```c
case UAC_CS_ENDPOINT: {
    const uac_as_cs_ep_desc_t *cs_ep_desc = (const uac_as_cs_ep_desc_t *)cs_desc;
    if (cs_ep_desc->bDescriptorSubtype == UAC_EP_GENERAL) {
        iface_alt->freq_ctrl_supported = cs_ep_desc->bmAttributes & UAC_SAMPLING_FREQ_CONTROL;
        parse_continue = false;  // Always stop here ❌
    }
    break;
}
```

**After:**
```c
case UAC_CS_ENDPOINT: {
    const uac_as_cs_ep_desc_t *cs_ep_desc = (const uac_as_cs_ep_desc_t *)cs_desc;
    if (cs_ep_desc->bDescriptorSubtype == UAC_EP_GENERAL) {
        iface_alt->freq_ctrl_supported = cs_ep_desc->bmAttributes & UAC_SAMPLING_FREQ_CONTROL;

        // Only stop if we already have the standard endpoint descriptor (ep_mps was set)
        // This handles non-compliant devices that place CS_ENDPOINT before standard ENDPOINT
        if (iface_alt->ep_mps != 0) {
            ESP_LOGD(TAG, "  Stopping parse (already have endpoint descriptor)");
            parse_continue = false;  // Standard endpoint already found ✓
        } else {
            ESP_LOGD(TAG, "  Continuing parse (waiting for standard endpoint descriptor)");
            // Continue parsing to find standard endpoint ✓
        }
    }
    break;
}
```

### Logic Flow

#### Compliant Device (CS_ENDPOINT after standard ENDPOINT)
1. Parse AS_GENERAL → extract format, terminal
2. Parse FORMAT_TYPE → extract channels, bit depth, frequencies
3. **Parse ENDPOINT** → `ep_mps = 192`, `ep_addr = 0x03`
4. Parse CS_ENDPOINT → `ep_mps != 0` → **stop parsing** ✓

#### Non-Compliant Device (CS_ENDPOINT before standard ENDPOINT)
1. Parse AS_GENERAL → extract format, terminal
2. Parse FORMAT_TYPE → extract channels, bit depth, frequencies
3. **Parse CS_ENDPOINT** → `ep_mps == 0` → **continue parsing** ✓
4. **Parse ENDPOINT** → `ep_mps = 192`, `ep_addr = 0x03` ✓

## Verification

### Expected Log Output (Jabra SPEAK 510)
```
D (1382) uac-host: --- Parsing alternate setting 1 (numEndpoints=1) ---
D (1387) uac-host:   UAC_AS_GENERAL: format=0x0001 terminalLink=1
D (1392) uac-host:   UAC_AS_FORMAT_TYPE_I: ch=2 bits=16 freq_count=3
D (1399) uac-host:     freq[0]=8000
D (1402) uac-host:     freq[1]=16000
D (1405) uac-host:     freq[2]=48000
D (1415) uac-host:   UAC_CS_ENDPOINT: subtype=EP_GENERAL attr=0x81 freqCtrl=1
D (1420) uac-host:   Continuing parse (waiting for standard endpoint descriptor)
D (1425) uac-host:   ENDPOINT: addr=0x03 (OUT/TX/Speaker) mps=192 attr=0x01 interval=1
D (1430) uac-host:   FEATURE_UNIT: unitID=2 volChMap=0x01 muteChMap=0x01
```

### Success Criteria
- `ep_mps = 192` (matches lsusb: wMaxPacketSize 0x00c0 = 192)
- `ep_addr = 0x03` (matches lsusb: bEndpointAddress 0x03 EP 3 OUT)
- Stream type correctly identified as TX/Speaker
- No assertion failure

## Tested Devices

✓ **Jabra SPEAK 510** (VID=0x0B0E PID=0x0422)
- Non-compliant descriptor ordering
- Speaker: Interface 1, EP 0x03 OUT, 192 bytes MPS
- Microphone: Interface 2, EP 0x83 IN, 32 bytes MPS

## References

- USB Audio Class 1.0 Specification: [Section 4.5.2 - Class-Specific AS Isochronous Audio Data Endpoint Descriptor](https://www.usb.org/sites/default/files/audio10.pdf)
- ESP-IDF USB Host Library: [Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s2/api-reference/peripherals/usb_host.html)
- Jabra SPEAK 510 lsusb output: [jabra510/lsusb.txt](jabra510/lsusb.txt)

## Impact

This fix enables compatibility with USB audio devices that place class-specific descriptors in non-standard order while maintaining compatibility with compliant devices. No performance impact, only affects descriptor parsing during device enumeration.
