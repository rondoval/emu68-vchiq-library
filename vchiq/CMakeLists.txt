# vchiq.device: the VCHIQ transport (freestanding, ROM-able).
project(vchiq.device VERSION ${CMAKE_PROJECT_VERSION})

set(VCHIQ_DEVICE_VERSTRING "$VER: ${PROJECT_NAME} ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR} ${CURRENT_DATE}")

# The public header alone (devices/vchiq.h), copied out of include/ so that
# videocore and the tools see nothing of the device's internals.
set(VCHIQ_PUBLIC_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/public-include)
configure_file(include/devices/vchiq.h ${VCHIQ_PUBLIC_INCLUDE_DIR}/devices/vchiq.h COPYONLY)
add_library(vchiq_headers INTERFACE)
target_include_directories(vchiq_headers INTERFACE ${VCHIQ_PUBLIC_INCLUDE_DIR})

# The transport core: free of the device (it is driven by the device's code).
set(VCHIQ_CORE_SOURCES
    src/vchiq_session.c
    src/vchiq_rx.c
    src/vchiq_tx.c
    src/vchiq_service.c
    src/vchiq_bulk.c
)

add_executable(vchiq_device
    src/device.c
    src/devtree_parse.c
    src/vchiq_link.c
    src/device_beginio.c
    src/device_abortio.c
    src/unit.c
    src/unit_task.c
    src/irq.c
    ${VCHIQ_CORE_SOURCES}
)

target_compile_definitions(vchiq_device PRIVATE
    DEVICE_IDSTRING="${VCHIQ_DEVICE_VERSTRING}"
    DEVICE_VERSION=${PROJECT_VERSION_MAJOR}
    DEVICE_REVISION=${PROJECT_VERSION_MINOR}
    DEVICE_NAME="${PROJECT_NAME}"
)
# NSCMD_VQ_TEST_ABANDON (vchiq/vchiq_test.h), for the test suite (tests/) only
if(VCHIQ_TEST_HOOKS)
    target_compile_definitions(vchiq_device PRIVATE VCHIQ_TEST_HOOKS)
endif()
target_compile_options(vchiq_device PRIVATE ${VCHIQ_MODULE_COMPILE_OPTIONS})
target_link_options(vchiq_device PRIVATE ${VCHIQ_MODULE_LINK_OPTIONS})

target_include_directories(vchiq_device
    PRIVATE
        include
)

target_link_libraries(vchiq_device
    PRIVATE
        Emu68Common::common
        GIC400::gic400_headers
)

# ROM-able: no writable .data/.bss may sneak in; the layout contract (entry at
# offset 0, RT_ENDSKIP, no writable sections) belongs to the linker.
emu68_module_layout(vchiq_device)

# The doorbell server answers with the Z flag; emu68_isr_z_check() slices it out
# of the LINKED module (by its section), so the check works with LTO or without.
# irq.c stays a real object because the server is file-scope asm(): there is no IR
# for LTO to carry, and which partition it would land in is unspecified.
emu68_enable_lto(vchiq_device)
emu68_lto_keep_real_objects(vchiq_device
    src/irq.c
)
emu68_isr_z_check(vchiq_device SERVERS vchiq_int_isr)

set_target_properties(vchiq_device PROPERTIES
    OUTPUT_NAME vchiq
    SUFFIX ".device"
)

install(TARGETS vchiq_device RUNTIME DESTINATION DEVS)
install(FILES include/devices/vchiq.h DESTINATION include/devices)
