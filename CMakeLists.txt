cmake_minimum_required(VERSION 3.18)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT DEFINED DMOD_MODULE_VERSION)
    set(DMOD_MODULE_VERSION "0.1" CACHE STRING "DMOD module version")
endif()

# If a local dmod checkout is provided, use it instead of fetching from git
if(DEFINED DMOD_DIR AND NOT "${DMOD_DIR}" STREQUAL "")
    set(FETCHCONTENT_SOURCE_DIR_DMOD "${DMOD_DIR}" CACHE PATH "" FORCE)
endif()

include(FetchContent)
FetchContent_Declare(
    dmod
    GIT_REPOSITORY https://github.com/choco-technologies/dmod.git
    GIT_TAG        develop
)

set(DMOD_MODE "DMOD_MODULE" CACHE STRING "DMOD build mode")
set(DMOD_BUILD_TESTS OFF CACHE BOOL "Build tests")
set(DMOD_BUILD_EXAMPLES OFF CACHE BOOL "Build examples")
set(DMOD_BUILD_TOOLS OFF CACHE BOOL "Build tools")
set(DMOD_BUILD_TEMPLATES OFF CACHE BOOL "Build templates")

FetchContent_MakeAvailable(dmod)

project(dmtcp VERSION ${DMOD_MODULE_VERSION}
    DESCRIPTION "dmtcp DMOD library module"
    LANGUAGES C CXX)

set(DMOD_DIR ${dmod_SOURCE_DIR} CACHE PATH "DMOD source directory")
set(DMOD_SCRIPTS_DIR ${DMOD_DIR}/scripts CACHE PATH "DMOD scripts directory")
include(${DMOD_DIR}/paths.cmake)
dmod_setup_external_module()

# Name of the module
set(DMOD_MODULE_NAME        dmtcp)

# Author (should be string)
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")

# Stack size for the module (should be integer)
set(DMOD_STACK_SIZE         1024)

# Path to the .dmr file - enables release package creation in build/packages/
set(DMOD_DMR_PATH           ${CMAKE_CURRENT_SOURCE_DIR}/dmtcp.dmr)

# List of MAL interfaces implemented by this module (leave empty if none)
set(DMOD_MAL_IMPLS          )

# List of DIF interfaces implemented by this module (leave empty if none)
set(DMOD_DIF_IMPLS          )

#
#   dmod_add_library - create a library module
#   it has the same signature as add_library
#   and can be used in the same way after the creation
#   (for example, to link libraries)
#
dmod_add_library(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    # List of source files - can include C and C++ files
    src/dmtcp.c
    src/dmtcp_registrations.c
    src/dmtcp_wire.c
    src/dmtcp_listen_table.c
    src/dmtcp_conn_table.c
    src/dmtcp_output.c
    src/dmtcp_input.c
    src/dmtcp_close.c
)

target_include_directories(${DMOD_MODULE_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(${DMOD_MODULE_NAME}_if INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# dmip:    dmip_send()/_v4_get_source_address() to transmit, dmip_checksum()
#          for the pseudo-header checksum, dmip_v4_parse_header()/
#          _v6_parse_header() to read the enclosing IP header,
#          dmip_register_protocol()/_unregister_protocol() to receive (see
#          src/dmtcp.c / src/dmtcp_input.c).
# dmroute: header-only - dmip.h needs dmroute_addr_t (re-exported as
#          dmip_addr_t) directly, even though dmtcp itself never calls a
#          dmroute_* function.
# dmnetif: header-only - dmnetif_iface_t, threaded through every handler/
#          accessor.
# dmlist:  backs the listen table (src/dmtcp_listen_table.c) and the
#          connection table (src/dmtcp_conn_table.c) - NOT the per-connection
#          send buffer, which is a plain byte stream, not a collection of
#          discrete items (see docs/dmtcp.md).
# dmosi:   mutexes (the listen table, the connection table, and one per TCB)
#          and, new relative to dmudp/dmicmp, dmosi_timer_t for the
#          retransmission timer and the TIME_WAIT timer, and
#          dmosi_get_tick_count() for initial sequence number generation.
#
# Deliberately NOT linked:
# dmicmp:     dmudp/dmicmp report "nobody's listening" via an ICMP message
#             because neither UDP nor plain IP has its own control
#             mechanism for that. TCP does: RFC 793's RST is dmtcp's own,
#             self-generated reply (built and sent through plain
#             dmip_send(), the same primitive dmicmp itself is built on)
#             for the "SYN to a closed port" / "segment for an unknown
#             connection" cases (see src/dmtcp_input.c's
#             send_rst_for_unmatched()) - there is no call site in this
#             module that needs a dmicmp_* function.
# dm_sw_ring: considered for the per-connection send buffer; rejected
#             because dmtcp already serializes all access to a TCB via its
#             own per-connection mutex (making dm_sw_ring's built-in
#             optional mutex/blocking-wait features redundant), and its
#             head-only peek/discard API doesn't cleanly support "peek the
#             already-sent-but-unacked prefix, discard exactly the acked
#             amount" without an extra copy anyway (see docs/dmtcp.md).
dmod_link_modules(${DMOD_MODULE_NAME}
    dmip
    dmroute
    dmnetif
    dmlist
    dmosi
)

enable_testing()
add_subdirectory(tests)
