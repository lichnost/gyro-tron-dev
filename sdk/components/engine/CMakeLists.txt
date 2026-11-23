idf_component_register(
    SRCS "gfx.cpp" "font5x7.cpp" "presenter.cpp" "input.cpp" "engine.cpp" "calib_fit.cpp" "autocal.cpp" "polar.cpp" "canvas.cpp" "store.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board esp_timer heap esp_hw_support esp_driver_gpio lodepng nvs_flash
)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
