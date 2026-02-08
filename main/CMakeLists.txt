# Build source list
set(COMPONENT_SRCS
    main.c
    core/input_scanner.c
    core/sd_card.c
    core/console.c
    core/display.cpp
    usb/msc.c
    player/player.c
    player/provider.c
    player/mapper.c
    player/persistent_volume.c
)

# Base component requirements
set(COMPONENT_REQUIRES
    fatfs
    spiffs
    nvs_flash
    esp_psram
    esp_driver_sdspi
    esp_driver_spi
    esp_driver_i2s
    usb
    usb_host_msc
    console
    esp_driver_gpio
    esp_timer
)

# Build private requires list (for components only needed by source files)
# PRIV_REQUIRES means it's only needed for implementation, not for the public interface
set(COMPONENT_PRIV_REQUIRES lcdgfx)

idf_component_register(
    SRCS ${COMPONENT_SRCS}
    INCLUDE_DIRS "." "core" "player" "usb"
    REQUIRES ${COMPONENT_REQUIRES}
    PRIV_REQUIRES ${COMPONENT_PRIV_REQUIRES}
)

# Create SPIFFS image from spiffs/ directory
spiffs_create_partition_image(spiffs ../spiffs FLASH_IN_PROJECT)
