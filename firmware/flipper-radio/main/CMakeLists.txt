set(radio_sources "radio.c" "wire.c" "midi_codec.c" "maintenance.c" "indicator.c" "indicator_pattern.c")
# IDF's early requirements pass does not inherit arbitrary -D project options.
# Keep the dependency unconditional; unused UART code is linker-collected in USB builds.
set(radio_private esp_wifi wpa_supplicant esp_timer nvs_flash esp_event app_update spi_flash mbedtls esp_driver_uart esp_driver_ledc)
if(LINK_STUDIO_GPIO)
    get_filename_component(ls_common "${CMAKE_CURRENT_LIST_DIR}/../../../flipper/common" ABSOLUTE)
    if(NOT EXISTS "${LINK_STUDIO_ZSTD}/lib/zstd.h")
        message(FATAL_ERROR "Run scripts/build-flipper-standalone.sh to prepare the pinned Zstd source")
    endif()
    set(ls_zstd "${LINK_STUDIO_ZSTD}/lib")
    list(APPEND radio_sources "../standalone/ls_ldn.c" "../standalone/ls_crypto_esp.c"
        "../standalone/ls_reliable.c" "../standalone/ls_pia.c" "../standalone/ls_connect.c"
        "${ls_common}/ls_wire.c"
        "${ls_zstd}/common/debug.c" "${ls_zstd}/common/entropy_common.c"
        "${ls_zstd}/common/error_private.c" "${ls_zstd}/common/fse_decompress.c"
        "${ls_zstd}/common/xxhash.c" "${ls_zstd}/common/zstd_common.c"
        "${ls_zstd}/decompress/huf_decompress.c" "${ls_zstd}/decompress/zstd_ddict.c"
        "${ls_zstd}/decompress/zstd_decompress.c" "${ls_zstd}/decompress/zstd_decompress_block.c")
endif()
idf_component_register(SRCS ${radio_sources} INCLUDE_DIRS "." "../standalone"
    PRIV_REQUIRES ${radio_private})
if(LINK_STUDIO_BUILD_ID)
    if(NOT LINK_STUDIO_BUILD_ID MATCHES "^[A-Za-z0-9._-]+$")
        message(FATAL_ERROR "Invalid firmware build identifier")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LS_BUILD_ID="${LINK_STUDIO_BUILD_ID}")
endif()
if(LINK_STUDIO_GPIO)
    target_include_directories(${COMPONENT_LIB} PRIVATE "${ls_common}" "${ls_zstd}")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LS_GPIO=1 ZSTD_DISABLE_ASM ZSTD_LEGACY_SUPPORT=0)
endif()
