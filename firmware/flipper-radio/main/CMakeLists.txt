set(radio_sources "radio.c" "wire.c" "midi_codec.c" "maintenance.c" "indicator.c" "indicator_pattern.c" "wifi_update.c" "update_contract.c" "wifi_profiles.c" "wifi_callbacks.c")
# IDF's early requirements pass does not inherit arbitrary -D project options.
# The release supports USB CDC, USB MIDI and the Flipper GPIO UART.
set(radio_private esp_wifi wpa_supplicant esp_timer nvs_flash esp_event app_update spi_flash mbedtls esp_driver_uart esp_driver_ledc esp_http_client esp_netif espressif__cjson)
if(LINK_STUDIO_GPIO)
    get_filename_component(ls_common "${CMAKE_CURRENT_LIST_DIR}/../../../flipper/common" ABSOLUTE)
    if(NOT EXISTS "${LINK_STUDIO_ZSTD}/lib/zstd.h")
        message(FATAL_ERROR "Run scripts/build-flipper-standalone.sh to prepare the pinned Zstd source")
    endif()
    set(ls_zstd "${LINK_STUDIO_ZSTD}/lib")
    list(APPEND radio_sources "../standalone/ls_ldn.c" "../standalone/ls_crypto_esp.c"
        "../standalone/ls_reliable.c" "../standalone/ls_stream.c" "../standalone/ls_session.c"
        "../standalone/ls_pia.c" "../standalone/ls_connect.c" "../standalone/ls_ip.c"
        "../standalone/ls_wifi_frame.c" "../standalone/ls_host.c"
        "${ls_common}/ls_wire.c" "${ls_common}/ls_game.c" "${ls_common}/ls_pa9.c"
        "${ls_common}/ls_recipe.c"
        "${ls_zstd}/common/debug.c" "${ls_zstd}/common/entropy_common.c"
        "${ls_zstd}/common/error_private.c" "${ls_zstd}/common/fse_decompress.c"
        "${ls_zstd}/common/xxhash.c" "${ls_zstd}/common/zstd_common.c"
        "${ls_zstd}/decompress/huf_decompress.c" "${ls_zstd}/decompress/zstd_ddict.c"
        "${ls_zstd}/decompress/zstd_decompress.c" "${ls_zstd}/decompress/zstd_decompress_block.c")
endif()
if(LINK_STUDIO_GPIO AND LINK_STUDIO_PROFILES_DIR)
    if(NOT EXISTS "${LINK_STUDIO_PROFILES_DIR}/ls_host_profiles.h")
        message(FATAL_ERROR "Prepare the bounded public Z-A host profiles before enabling runtime")
    endif()
    list(APPEND radio_sources "../standalone/runtime.c")
endif()
idf_component_register(SRCS ${radio_sources} INCLUDE_DIRS "." "../standalone"
    PRIV_REQUIRES ${radio_private})
target_compile_definitions(${COMPONENT_LIB} PRIVATE LS_GPIO=1)
if(LINK_STUDIO_BUILD_ID)
    if(NOT LINK_STUDIO_BUILD_ID MATCHES "^[A-Za-z0-9._-]+$")
        message(FATAL_ERROR "Invalid firmware build identifier")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LS_BUILD_ID="${LINK_STUDIO_BUILD_ID}")
endif()
if(LINK_STUDIO_GPIO)
    target_include_directories(${COMPONENT_LIB} PRIVATE "${ls_common}" "${ls_zstd}")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ZSTD_DISABLE_ASM ZSTD_LEGACY_SUPPORT=0)
endif()
if(LINK_STUDIO_GPIO AND LINK_STUDIO_PROFILES_DIR)
    target_include_directories(${COMPONENT_LIB} PRIVATE "${LINK_STUDIO_PROFILES_DIR}")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LS_STANDALONE=1)
endif()
