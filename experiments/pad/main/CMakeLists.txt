# The pad reuses the hub's HID service (keyboard + mouse + consumer reports) unchanged.
idf_component_register(SRCS "main.c" "../../../main/hid_dev.c"
                       INCLUDE_DIRS "." "../../../main"
                       PRIV_REQUIRES bt nvs_flash driver esp_timer)
