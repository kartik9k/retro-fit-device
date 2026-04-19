idf_component_register(
    SRCS "app_main.c"
         "hcsr04.c"
    INCLUDE_DIRS "."
    REQUIRES driver
)
