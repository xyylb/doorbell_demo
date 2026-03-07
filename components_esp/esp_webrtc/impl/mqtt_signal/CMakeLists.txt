
file(GLOB CODES "mqtt_signaling.cpp")

idf_component_register(
    SRCS ${CODES}
    INCLUDE_DIRS "./include"
    REQUIRES json mqtt esp-ml307
)
