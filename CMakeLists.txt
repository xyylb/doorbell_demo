# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# include($ENV{ADF_PATH}/CMakeLists.txt)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

if("${IDF_TARGET}" STREQUAL "esp32p4")
    set(EXTRA_COMPONENT_DIRS "$ENV{IDF_PATH}/examples/ethernet/basic/components/ethernet_init")
endif()

list(APPEND EXTRA_COMPONENT_DIRS "./components")

list(APPEND EXTRA_COMPONENT_DIRS "./components_esp")

list(APPEND EXTRA_COMPONENT_DIRS "./common")

project(doorbell_demo)

# Add compiler flags to ignore certain warnings for main component
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-error")

idf_build_set_property(COMPONENT_LIBS 
    esp_ml307           # 你的4G组件放在最前面
    esp_peer            # 然后是esp_peer组件
    main         # 主程序
    # 其他组件按需添加...
    CACHE INTERNAL ""
)