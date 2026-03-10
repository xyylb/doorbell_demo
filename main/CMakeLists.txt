idf_component_register(SRCS "main.cpp" "board.c" "media_sys.c" "webrtc_mqtt.c" "mqtt_signaling.cpp" "app_config.c"
 "network_4g.cpp" "ml307_udp_socket.cc" "door_bell_audio.c"
                       EMBED_TXTFILES "ring.aac" "open.aac" "join.aac"
                       INCLUDE_DIRS ".")
