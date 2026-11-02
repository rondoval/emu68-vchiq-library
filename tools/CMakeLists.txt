# Hosted CLI tools (-mcrt=nix20, the toolchain default): the freestanding flags of
# the two modules are their own and do not reach this directory.
#
#   VCInfo    the user's check: the setup in plain words, what the codecs do,
#             gencmd, the firmware's MMAL components, the connection's counters
#             and the shared window (vchiq.device, videocodec.library)
#   VCTest    the test rig the suite drives (tests/): one command per run,
#             facts as key=value (STATUS, CODECS, DECODE, IMAGE, AUDIO, ...)
#   VPUPlay   the test player: a decoder session into a Picasso96 screen or
#             window (videocodec.library); the measurement rig of the display path
#   VCGrab    the user's recorder: an RTG screen through the converter into the
#             H.264 encoder and a Matroska file, JPEG stills, a commodity
#   VCPlay    the session example: the smallest player on videocodec.library, a
#             window on the Workbench, the code to copy from (shipped as source)
#   VCShow    the image-call example: the smallest picture viewer (shipped as source)
#
# common/ is the tools' shared code: a VCHIQ service as Exec I/O
# (vchiq_client.c), codecs and formats by name (vc_names.c), the output
# grammar (kv.c) and the debug-log writer (debugport.c), the task, signal
# and timer a tool waits with (waiter.c), ReadArgs() defaults (args.h), MD5 (md5.h).
# The examples link none of it.  The Picasso96 API headers (P96Develop) come
# with the toolchain image.

set(VCHIQ_TOOL_COMPILE_OPTIONS
    -Os
    -Wall
    -Wextra
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wmissing-prototypes
    -Wstrict-prototypes
)

add_library(tools_common STATIC
    common/vchiq_client.c
    common/vc_names.c
    common/kv.c
    common/debugport.c
    common/waiter.c
    common/pixel_format.c
)
target_include_directories(tools_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_compile_options(tools_common PRIVATE ${VCHIQ_TOOL_COMPILE_OPTIONS})
# drv_timer and the string helpers from emu68-common
target_link_libraries(tools_common PUBLIC vchiq_headers videocodec_headers Emu68Common::common)

add_executable(VCInfo
    VCInfo.c
)
# the window layout (vchiq_core.h) for DUMP, and MMAL's values and wire format
# (mmal.h, mmal_proto.h) for COMPONENTS: read-only, a diagnostic tool's privilege
target_include_directories(VCInfo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../vchiq/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../videocore/include
)
target_compile_options(VCInfo PRIVATE ${VCHIQ_TOOL_COMPILE_OPTIONS})
target_compile_definitions(VCInfo PRIVATE
    VERSTAG="$VER: VCInfo ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR} ${CURRENT_DATE}"
)
target_link_options(VCInfo PRIVATE -s)
target_link_libraries(VCInfo PRIVATE tools_common)

add_executable(VCTest
    vctest/vctest.c
    vctest/status.c
    vctest/abandon.c
    vctest/audio.c
    vctest/ahi.c
    vctest/decode.c
    vctest/picture.c
    vctest/image.c
    vctest/datatype.c
    vctest/encode.c
    vctest/screen.c
    vctest/hold.c
)
# the test-only command's number (vchiq/vchiq_test.h): a test rig's privilege; the AUDS wire format
target_include_directories(VCTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../vchiq/include
    ${VCHIQ_SHARED_DIR}/include
)
target_compile_options(VCTest PRIVATE ${VCHIQ_TOOL_COMPILE_OPTIONS})
target_compile_definitions(VCTest PRIVATE
    VERSTAG="$VER: VCTest ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR} ${CURRENT_DATE}"
)
target_link_options(VCTest PRIVATE -s)
target_link_libraries(VCTest PRIVATE tools_common)

add_executable(VPUPlay
    VPUPlay.c
)
target_compile_options(VPUPlay PRIVATE ${VCHIQ_TOOL_COMPILE_OPTIONS})
target_compile_definitions(VPUPlay PRIVATE
    VERSTAG="$VER: VPUPlay ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR} ${CURRENT_DATE}"
)
target_link_options(VPUPlay PRIVATE -s)
target_link_libraries(VPUPlay PRIVATE tools_common)

add_executable(VCGrab
    vcgrab/vcgrab.c
    vcgrab/commodity.c
    vcgrab/settings.c
    vcgrab/window.c
    vcgrab/screen.c
    vcgrab/record.c
    vcgrab/mkv/mkv.c
    vcgrab/still.c
)
target_compile_options(VCGrab PRIVATE ${VCHIQ_TOOL_COMPILE_OPTIONS})
target_include_directories(VCGrab PRIVATE vcgrab/mkv) # the mkv-writer submodule
target_compile_definitions(VCGrab PRIVATE
    VERSTAG="$VER: VCGrab ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR} ${CURRENT_DATE}"
)
target_link_options(VCGrab PRIVATE -s)
target_link_libraries(VCGrab PRIVATE tools_common amiga) # HotKey() lives in amiga.lib

# The examples need nothing of the stack but the library's headers.
add_executable(VCPlay
    VCPlay.c
)
target_compile_options(VCPlay PRIVATE ${VCHIQ_TOOL_COMPILE_OPTIONS})
target_compile_definitions(VCPlay PRIVATE
    VERSTAG="$VER: VCPlay ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR} ${CURRENT_DATE}"
)
target_link_options(VCPlay PRIVATE -s)
target_link_libraries(VCPlay PRIVATE videocodec_headers)

add_executable(VCShow
    VCShow.c
)
target_compile_options(VCShow PRIVATE ${VCHIQ_TOOL_COMPILE_OPTIONS})
target_compile_definitions(VCShow PRIVATE
    VERSTAG="$VER: VCShow ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR} ${CURRENT_DATE}"
)
target_link_options(VCShow PRIVATE -s)
target_link_libraries(VCShow PRIVATE videocodec_headers)

# C: the user's tools (build.sh uploads C/).  Developer/ goes into the archive
# only: the test rig and the test player, and the examples with their source
# (the test suite uploads them to RAM: for its runs).
install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/VCInfo DESTINATION C)
install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/VCGrab DESTINATION Tools/Commodities)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../dist/VCGrab.info DESTINATION Tools/Commodities)
install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/VCTest ${CMAKE_CURRENT_BINARY_DIR}/VPUPlay DESTINATION Developer/Tools)
install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/VCPlay ${CMAKE_CURRENT_BINARY_DIR}/VCShow DESTINATION Developer/Examples/videocodec)
install(FILES VCPlay.c VCShow.c DESTINATION Developer/Examples/videocodec)
install(FILES examples.readme DESTINATION Developer/Examples/videocodec RENAME ReadMe)
