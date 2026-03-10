if(CONFIG_SYSTEM_GREP)
  nuttx_add_application(
    MODULE ${CONFIG_SYSTEM_GREP}
    NAME grep
    SRCS grep_main.c
    STACKSIZE ${CONFIG_SYSTEM_GREP_STACKSIZE}
  )
endif()
