idf_component_register(
    SRCS "pb_hil.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_usb_serial_jtag json pb_fan pb_leds pb_ntc pb_policy pb_buttons
)
