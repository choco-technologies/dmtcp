# Name of the module
set(DMOD_MODULE_NAME        tcp_echo)

# Author (should be string)
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")

# Stack size for the module (should be integer)
set(DMOD_STACK_SIZE         1024)

# Priority for the thread of the module (should be integer)
set(DMOD_PRIORITY           0)

#
#   dmod_add_executable - create an executable module
#   it has the same signature as add_executable
#   and can be used in the same way after the creation
#   (for example, to link libraries)
#
dmod_add_executable(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    src/tcp_echo.c
)

# dmtcp is this same repository's own top-level module, already built as
# part of this CMake configure by the time tools/ is added (see the root
# CMakeLists.txt) - link its dmtcp_if target directly, exactly like
# tests/CMakeLists.txt does, instead of fetching dmtcp's own published
# headers back via dmod_link_modules() as if it were an external module.
target_link_libraries(${DMOD_MODULE_NAME} dmtcp_if)

# dmip/dmroute/dmnetif: header-only - dmtcp.h #includes dmip.h directly
#        (dmip_addr_t), same reasoning dmicmp/tools/ping and
#        dmudp/tools/udp_echo give for linking them without calling their
#        functions directly.
# dmosi: dmosi_thread_sleep() to keep the listener alive for the run.
dmod_link_modules(${DMOD_MODULE_NAME}
    dmip
    dmroute
    dmnetif
    dmosi
)
