cmake_minimum_required(VERSION 3.14.0)
project(emu68-vchiq VERSION 0.3)
# The one version of the repository: every binary takes it (project(... VERSION
# ${CMAKE_PROJECT_VERSION}) in its directory, the tools directly), the sources have no
# number of their own and the SFDs carry none.  Bump it here only.

# Five binaries and their test tools:
#   vchiq/       vchiq.device        the VCHIQ transport to the VPU firmware (DEVS:)
#   videocodec/  videocodec.library  the public codec API, backend-agnostic (LIBS:),
#                                    and the backend ABI every backend builds against
#   videocore/   videocore.codec     the MMAL client on top of vchiq.device, the first
#                                    backend: codecs, ISP (LIBS:videocodec/)
#   ahi/         videocore.audio     an AHI sub-driver on the firmware's audio service
#                                    (DEVS:AHI/, its mode file in DEVS:AudioModes/)
#   datatype/    vcjpeg.datatype     a JPEG picture class on videocodec.library
#                                    (SYS:Classes/DataTypes/, its descriptor VCJPEG in
#                                    SYS:Storage/DataTypes/, added from S:User-Startup)
#   tools/       VCInfo, VCGrab (C:); VCTest and VPUPlay, the test rig (Developer/Tools);
#                VCPlay and VCShow, the examples (Developer/Examples/videocodec, with
#                their sources)

# vchiq.device's test-only commands (vchiq/vchiq_test.h), for the hardware test
# suite (tests/); never in a release.  The stack forwards
# EMU68_VCHIQ_TEST_HOOKS here.
option(VCHIQ_TEST_HOOKS "vchiq.device: the test-only commands of the test suite (tests/)" OFF)


# Version strings (stack convention, see emu68-gic400-library): "$VER: name x.y (dd.mm.yyyy)"
string(TIMESTAMP CURRENT_DATE "(%d.%m.%Y)")
if(EMU68_BUILD_STAMP)   # local-build marker from the stack's build.sh; empty in releases
    string(APPEND CURRENT_DATE " ${EMU68_BUILD_STAMP}")
endif()

# Dependencies: emu68-common (devtree helpers, driver task, reset guard, cache
# ops, MMIO/timing headers, dma_mem, drv_waitq) and gic400.library's headers (the
# doorbell interrupt server).  The client headers of mailbox.resource (the VCHIQ init
# tag goes through MB_RawCommand) and cma.resource (the window), and the Picasso96 API
# headers of the players, come with the toolchain image (amiga-build-container).
find_package(Emu68Common 2.3 CONFIG REQUIRED) # 2.3: prefs_open(), strfit(); 2.1: drv_waitq, the dma-ranges/symbol/carve-out devtree helpers
find_package(GIC400 1.3 CONFIG REQUIRED)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/install" CACHE PATH "Install path" FORCE)
endif()

# Debug-output backend defines (EMU68_DEBUG_BACKEND, see emu68-common).
emu68_debug_definitions()
# cache_ops.h LVO-fallback define (EMU68_FORCE_LVO_CACHE_OPS, see emu68-common).
emu68_cache_ops_definitions()

# Warnings and code generation shared by the three freestanding binaries
# (CPU/FPU/frame-pointer flags come from the stack toolchain).
set(VCHIQ_MODULE_COMPILE_OPTIONS
    -O3
    -ffreestanding
    -Wall
    -Wextra
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wmissing-prototypes
    -Wstrict-prototypes
)
set(VCHIQ_MODULE_LINK_OPTIONS
    -nostdlib
    -nostartfiles
    -Wall
    -Wextra
    -s
)

# Sources built into more than one binary: a still picture's header (image_info.c),
# for videocodec.library's VC_GetImageInfo() and videocore.codec's checks; the AUDS
# wire format (auds_proto.h), for videocore.audio and VCTest.
set(VCHIQ_SHARED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shared)

add_subdirectory(vchiq)
add_subdirectory(videocodec)
add_subdirectory(videocore)
add_subdirectory(ahi)
add_subdirectory(datatype)
add_subdirectory(tools)
