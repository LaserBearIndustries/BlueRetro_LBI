idf_component_register(SRCS "si_sniffer.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_driver_gpio esp_driver_uart hal soc)

# Which console port to listen to, chosen at build time:
#   idf.py -DSI_PORT=2 build
if(DEFINED SI_PORT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SI_PORT=${SI_PORT})
endif()
