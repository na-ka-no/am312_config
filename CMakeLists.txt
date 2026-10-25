if(CONFIG_ZMK_BEHAVIOR_PIR_SWITCH)
  target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_sources(app PRIVATE src/behavior_pir_switch.c)
endif()

target_sources_ifdef(CONFIG_ZMK_BATTERY_NIMH_VOLTAGE_DIVIDER app PRIVATE src/battery_nimh.c)
