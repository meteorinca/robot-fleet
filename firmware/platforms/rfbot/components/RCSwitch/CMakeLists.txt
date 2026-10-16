set(component_srcs "RCSwitch.c" "rf_codec.c" "rf_rmt.c")

idf_component_register(
	SRCS "${component_srcs}"
	PRIV_REQUIRES esp_driver_gpio esp_timer esp_driver_rmt
	INCLUDE_DIRS "."
)
