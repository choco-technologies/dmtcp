set(DMOD_MODULE_NAME        test_dmtcp)
set(DMOD_MODULE_VERSION     ${PROJECT_VERSION})
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")
set(DMOD_STACK_SIZE         1024)
set(DMOD_PRIORITY           0)

dmod_add_test(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    dmtcp_test.c
)

target_include_directories(${DMOD_MODULE_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

target_link_libraries(${DMOD_MODULE_NAME} dmtcp_if)

# dmip/dmnetbridge/dmarp/dmnetif/dmroute: drive dmtcp's registered
# packet_received DIF implementation (via dmip, transitively through
# dmnetbridge) with hand-built fixture packets, and seed fixture routes/ARP
# entries for the send-path tests - the same set dmudp_test.c/dmicmp_test.c
# link for the same reason (see their own tests/CMakeLists.txt comments).
# dmosi: the test itself calls dmosi_get_tick_count() (to predict the
#        server's ISS) and dmosi_thread_sleep() (to wait out the
#        retransmission retry limit).
dmod_link_modules(${DMOD_MODULE_NAME}
    dmip
    dmnetbridge
    dmarp
    dmnetif
    dmroute
    dmosi
)

# Register the test module with ctest - this replays the same two steps CI
# runs (see .github/templates/ci.yml.template): install the test module's
# dependencies with dmf-get, then execute it through dmod_loader. The test's
# exit code equals its number of failed steps (see dmod_add_test).
find_program(DMF_GET_EXECUTABLE dmf-get HINTS ${DMOD_TOOLS_BIN_DIR} PATHS ENV PATH)
find_program(DMOD_LOADER_EXECUTABLE dmod_loader HINTS ${DMOD_TOOLS_BIN_DIR} PATHS ENV PATH)

if(DMF_GET_EXECUTABLE AND DMOD_LOADER_EXECUTABLE)
    add_test(
        NAME ${DMOD_MODULE_NAME}
        COMMAND ${CMAKE_COMMAND} -E env DMOD_DMF_DIR=${DMOD_DMF_DIR}
            bash -c "${DMF_GET_EXECUTABLE} install -d ${DMOD_DMF_DIR}/${DMOD_MODULE_NAME}-local.dmd -y && ${DMOD_LOADER_EXECUTABLE} ${DMOD_DMF_DIR}/${DMOD_MODULE_NAME}.dmf"
    )
else()
    message(WARNING "dmf-get/dmod_loader not found - 'ctest' will not run ${DMOD_MODULE_NAME}. Build the tools first with -DDMOD_BUILD_TOOLS=ON")
endif()

# Run the real segment/state-machine code with a scripted ARP cache miss.
if(DMOD_TOOLS_NAME STREQUAL "arch/x86_64")
    add_executable(test_tcp_arp tcp_arp_test.c ../src/dmtcp_wire.c)
    target_compile_definitions(test_tcp_arp PRIVATE DMOD_dmtcp DMOD_CURRENT_ALLOCATOR="test_tcp_arp")
    get_target_property(TCP_INCLUDES dmtcp INCLUDE_DIRECTORIES)
    target_include_directories(test_tcp_arp PRIVATE ${TCP_INCLUDES})
    target_link_libraries(test_tcp_arp PRIVATE dmtcp_if dmod_inc)
    target_compile_options(test_tcp_arp PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_tcp_arp PRIVATE -Wl,--gc-sections)
    add_test(NAME tcp_arp COMMAND test_tcp_arp)
endif()
