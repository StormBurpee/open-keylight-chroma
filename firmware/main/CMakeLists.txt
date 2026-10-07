idf_component_register(SRCS "app.c" "flash_guard.c" "storage.c" "scene_store.c" "network.c" "state_json.c" "http_server.c"
    "config_api.c" "update.c" "update_indicator.c" "update_indicator_output.c" "controller_update.c" "controller_job.c" "controller_worker.c"
    "worker.c" "output_policy.c" "nxp_transport.c" "button.c" "mqtt.c"
    INCLUDE_DIRS "." "${CMAKE_CURRENT_BINARY_DIR}"
    REQUIRES keylight_core keylight_nxp keylight_loader keylight_json esp_wifi esp_netif esp_http_server app_update nvs_flash
        json mqtt mbedtls esp_timer esp_driver_gpio esp_driver_spi mdns)
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../../dashboard/dist/asset-manifest.json")
  set(WEB_HEADER "${CMAKE_CURRENT_BINARY_DIR}/web_assets.h")
  execute_process(COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/../../tools/embed_dashboard.py" "${WEB_HEADER}"
      RESULT_VARIABLE EMBED_RESULT)
  if(NOT EMBED_RESULT EQUAL 0)
    message(FATAL_ERROR "Build the dashboard first: cd dashboard && npm ci && npm run build")
  endif()
endif()
