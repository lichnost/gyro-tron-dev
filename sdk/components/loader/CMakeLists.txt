# Loading a game that was not built into the firmware (docs/GAME_API.md sections 2 and 6).
# sig.c/keys.c are the install-time signature verifier: plain portable C, no ESP-IDF
# includes, so the emulator's loader probe and the host tests compile them unchanged.
idf_component_register(
    SRCS "loader.c" "exports.c" "packaged_game.cpp" "sig.c" "keys.c"
    INCLUDE_DIRS "include"
    REQUIRES tat_api storage esp_mm esp_rom engine wc_console
)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
