idf_component_register(
  SRC_DIRS
    "src"
    "lib/zeropack/src"
  INCLUDE_DIRS
    "include"
    "lib/zeropack/include"
  REQUIRES
    lwip esp_netif freertos
)
