## esp-usb Component API Reference

This section documents the esp-usb component APIs used by this project for improved code lookup and development assistance.

**Repository:** `$HOME/projets/esp-idf/esp-usb/`
**Branch:** `uac-jabra` (local fork with Jabra SPEAK 510 fixes)

### UAC (USB Audio Class) Host Driver

**Location:** `host/class/uac/usb_host_uac/`
**Version:** 1.3.3
**Headers:** `include/usb/uac_host.h`, `include/usb/uac.h`

#### Initialization & Cleanup

```c
esp_err_t uac_host_install(const uac_host_driver_config_t *config);
esp_err_t uac_host_uninstall(void);
esp_err_t uac_host_handle_events(TickType_t timeout);  // Call from task if create_background_task=false
```

#### Device Management

```c
esp_err_t uac_host_device_open(const uac_host_device_config_t *config,
                                uac_host_device_handle_t *uac_dev_handle);
esp_err_t uac_host_device_close(uac_host_device_handle_t uac_dev_handle);
esp_err_t uac_host_get_device_info(uac_host_device_handle_t uac_dev_handle,
                                   uac_host_dev_info_t *uac_dev_info);
esp_err_t uac_host_get_device_alt_param(uac_host_device_handle_t uac_dev_handle,
                                        uint8_t iface_alt,
                                        uac_host_dev_alt_param_t *uac_alt_param);
esp_err_t uac_host_printf_device_param(uac_host_device_handle_t uac_dev_handle);
```

#### Stream Control

```c
esp_err_t uac_host_device_start(uac_host_device_handle_t uac_dev_handle,
                                const uac_host_stream_config_t *stream_config);
esp_err_t uac_host_device_stop(uac_host_device_handle_t uac_dev_handle);
esp_err_t uac_host_device_suspend(uac_host_device_handle_t uac_dev_handle);
esp_err_t uac_host_device_resume(uac_host_device_handle_t uac_dev_handle);
```

#### Audio Data Transfer

```c
esp_err_t uac_host_device_read(uac_host_device_handle_t uac_dev_handle,
                               uint8_t *data, uint32_t size,
                               uint32_t *bytes_read, uint32_t timeout);
esp_err_t uac_host_device_write(uac_host_device_handle_t uac_dev_handle,
                                uint8_t *data, uint32_t size, uint32_t timeout);
```

#### Volume & Mute Control

```c
esp_err_t uac_host_device_set_mute(uac_host_device_handle_t uac_dev_handle, bool mute);
esp_err_t uac_host_device_get_mute(uac_host_device_handle_t uac_dev_handle, bool *mute);
esp_err_t uac_host_device_set_volume(uac_host_device_handle_t uac_dev_handle, uint8_t volume);
esp_err_t uac_host_device_get_volume(uac_host_device_handle_t uac_dev_handle, uint8_t *volume);
esp_err_t uac_host_device_set_volume_db(uac_host_device_handle_t uac_dev_handle, int16_t volume_db);
esp_err_t uac_host_device_get_volume_db(uac_host_device_handle_t uac_dev_handle, int16_t *volume_db);
esp_err_t uac_host_device_get_volume_range(uac_host_device_handle_t uac_dev_handle,
                                           int16_t *min_db, int16_t *max_db, int16_t *res_db);
```

#### UAC Configuration Structures

```c
// Driver configuration
typedef struct {
    bool create_background_task;                // Auto-create event handling task
    size_t task_priority;                       // Task priority (if created)
    size_t stack_size;                          // Task stack size
    BaseType_t core_id;                         // CPU core affinity
    uac_host_driver_event_cb_t callback;        // Driver event callback (required)
    void *callback_arg;                         // Callback user argument
} uac_host_driver_config_t;

// Device configuration
typedef struct {
    uint8_t addr;                               // USB device address
    uint8_t iface_num;                          // UAC interface number
    uint32_t buffer_size;                       // Audio buffer size
    uint32_t buffer_threshold;                  // Buffer threshold for events
    uac_host_device_event_cb_t callback;        // Device event callback
    void *callback_arg;                         // Callback user argument
} uac_host_device_config_t;

// Stream configuration
typedef struct {
    uint8_t channels;                           // Channel count (1=mono, 2=stereo)
    uint8_t bit_resolution;                     // Bits per sample (typically 16)
    uint32_t sample_freq;                       // Sample rate in Hz
    uint16_t flags;                             // Control flags
} uac_host_stream_config_t;
```

#### UAC Event Types

```c
// Driver-level events (callback: uac_host_driver_event_cb_t)
typedef enum {
    UAC_HOST_DRIVER_EVENT_RX_CONNECTED = 0x00,  // Microphone device connected
    UAC_HOST_DRIVER_EVENT_TX_CONNECTED,         // Speaker device connected
} uac_host_driver_event_t;

// Device-level events (callback: uac_host_device_event_cb_t)
typedef enum {
    UAC_HOST_DEVICE_EVENT_RX_DONE = 0x00,       // RX buffer threshold exceeded
    UAC_HOST_DEVICE_EVENT_TX_DONE,              // TX buffer below threshold
    UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR,       // Transfer error occurred
    UAC_HOST_DRIVER_EVENT_DISCONNECTED,         // Device disconnected
} uac_host_device_event_t;
```

#### UAC Descriptor Constants (from uac.h)

```c
// Subclass codes
#define UAC_SUBCLASS_AUDIOCONTROL   0x01        // Audio control interface
#define UAC_SUBCLASS_AUDIOSTREAMING 0x02        // Audio streaming interface

// Descriptor types
#define UAC_CS_INTERFACE            0x24        // Class-specific interface descriptor
#define UAC_CS_ENDPOINT             0x25        // Class-specific endpoint descriptor

// Feature unit control selectors
#define UAC_MUTE_CONTROL            0x01
#define UAC_VOLUME_CONTROL          0x02
```

### MSC (Mass Storage Class) Host Driver

**Location:** `host/class/msc/usb_host_msc/`
**Version:** 1.1.4
**Headers:** `include/usb/msc_host.h`, `include/usb/msc_host_vfs.h`

#### Initialization & Cleanup

```c
esp_err_t msc_host_install(const msc_host_driver_config_t *config);
esp_err_t msc_host_uninstall(void);
esp_err_t msc_host_handle_events(uint32_t timeout);  // Call from task if create_backround_task=false
```

#### Device Management

```c
esp_err_t msc_host_install_device(uint8_t device_address, msc_host_device_handle_t *device);
esp_err_t msc_host_uninstall_device(msc_host_device_handle_t device);
esp_err_t msc_host_get_device_info(msc_host_device_handle_t device, msc_host_device_info_t *info);
esp_err_t msc_host_print_descriptors(msc_host_device_handle_t device);
esp_err_t msc_host_reset_recovery(msc_host_device_handle_t device);
```

#### VFS Integration (Recommended)

```c
esp_err_t msc_host_vfs_register(msc_host_device_handle_t device,
                                const char *base_path,
                                const esp_vfs_fat_mount_config_t *mount_config,
                                msc_host_vfs_handle_t *vfs_handle);
esp_err_t msc_host_vfs_unregister(msc_host_vfs_handle_t vfs_handle);
```

#### MSC Configuration Structures

```c
// Driver configuration
typedef struct {
    bool create_backround_task;                 // Auto-create event handling task (note: typo in original)
    size_t task_priority;                       // Task priority
    size_t stack_size;                          // Task stack size
    BaseType_t core_id;                         // CPU core affinity
    msc_host_event_cb_t callback;               // Event callback (required)
    void *callback_arg;                         // Callback user argument
} msc_host_driver_config_t;

// Device information
typedef struct {
    uint32_t sector_count;                      // Total sectors on device
    uint32_t sector_size;                       // Bytes per sector (typically 512)
    uint16_t idProduct;                         // USB Product ID
    uint16_t idVendor;                          // USB Vendor ID
    wchar_t iManufacturer[32];                  // Manufacturer string
    wchar_t iProduct[32];                       // Product string
    wchar_t iSerialNumber[32];                  // Serial number
} msc_host_device_info_t;
```

#### MSC Event Types

```c
typedef struct {
    enum {
        MSC_DEVICE_CONNECTED,                   // Device connected
        MSC_DEVICE_DISCONNECTED,                // Device disconnected
    } event;
    union {
        uint8_t address;                        // Address for connect event
        msc_host_device_handle_t handle;        // Handle for disconnect event
    } device;
} msc_host_event_t;

typedef void (*msc_host_event_cb_t)(const msc_host_event_t *event, void *arg);
```

### Common Usage Patterns

**UAC Playback Flow (used in [main/uac.c](main/uac.c)):**
```c
// 1. Install USB host library (usb_host_install)
// 2. Install UAC driver
uac_host_install(&uac_config);
// 3. Wait for UAC_HOST_DRIVER_EVENT_TX_CONNECTED in callback
// 4. Open device
uac_host_device_open(&dev_config, &handle);
// 5. Configure and start stream
uac_host_device_start(handle, &stream_config);
// 6. Write audio data in loop
uac_host_device_write(handle, pcm_data, size, timeout);
// 7. Cleanup on disconnect
uac_host_device_stop(handle);
uac_host_device_close(handle);
```

**MSC Sync Flow (used in [main/msc.c](main/msc.c)):**
```c
// 1. Install USB host library (usb_host_install)
// 2. Install MSC driver
msc_host_install(&msc_config);
// 3. Wait for MSC_DEVICE_CONNECTED in callback
// 4. Install device
msc_host_install_device(addr, &device);
// 5. Register VFS
msc_host_vfs_register(device, "/usb", &mount_config, &vfs_handle);
// 6. Access files via standard POSIX APIs (fopen, fread, etc.)
// 7. Cleanup
msc_host_vfs_unregister(vfs_handle);
msc_host_uninstall_device(device);
```

**Deferred Initialization Pattern (avoids reentrancy):**
The UAC/MSC driver callbacks run in the driver task context. To avoid calling driver APIs from callbacks (which can cause reentrancy issues), signal an event group and handle initialization in the main loop:
```c
// In callback (driver task context):
xEventGroupSetBits(event_group, DEVICE_CONNECTED_BIT);

// In main loop (app_main context):
xEventGroupWaitBits(event_group, DEVICE_CONNECTED_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
init_uac_application();  // Safe to call driver APIs here