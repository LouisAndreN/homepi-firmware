# Status LED patterns and NTP sync, identical in the three firmware projects
# (they were copied in each project; a fix had to be made three times).
idf_component_register(
    SRCS
        "src/led.c"
        "src/ntp_sync.c"
    INCLUDE_DIRS "include"
    REQUIRES driver lwip
    PRIV_REQUIRES log
)
