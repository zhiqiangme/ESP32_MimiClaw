idf_component_register(
    SRCS
        "led.c"
        "key.c"
        "uart.c"
        "exit.c"
    INCLUDE_DIRS "."
    REQUIRES driver
)

target_compile_options(${COMPONENT_LIB} PRIVATE -ffast-math -O3 -Wno-format)
