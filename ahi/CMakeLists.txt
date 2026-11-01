# videocore.audio: an AHI sub-driver on the VPU firmware's audio service
# (freestanding).  Its function table is AHI's sub-driver interface
# (ahi_sub.sfd, the AHI developer kit's headers in the toolchain image); its
# modes come from the mode file modefile.py writes.
project(videocore.audio VERSION ${CMAKE_PROJECT_VERSION})

set(VIDEOCORE_AUDIO_VERSTRING "$VER: ${PROJECT_NAME} ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR} ${CURRENT_DATE}")

add_executable(videocore_audio
    src/audio_lib.c
    src/audio_sub.c
    src/audio_slave.c
)
# lib_Version is AHI's driver interface (videocore_audio.h), so only the revision and the string come from here.
target_compile_definitions(videocore_audio PRIVATE
    LIBRARY_IDSTRING="${VIDEOCORE_AUDIO_VERSTRING}"
    LIBRARY_REVISION=${PROJECT_VERSION_MINOR}
    LIBRARY_NAME="${PROJECT_NAME}"
)
target_compile_options(videocore_audio PRIVATE ${VCHIQ_MODULE_COMPILE_OPTIONS})
target_link_options(videocore_audio PRIVATE ${VCHIQ_MODULE_LINK_OPTIONS})
# vchiq.device's public header and the AUDS wire format only.
target_include_directories(videocore_audio PRIVATE
    include
    ${VCHIQ_SHARED_DIR}/include
)
target_link_libraries(videocore_audio PRIVATE vchiq_headers Emu68Common::common)

emu68_module_layout(videocore_audio)
emu68_enable_lto(videocore_audio)

set_target_properties(videocore_audio PROPERTIES
    OUTPUT_NAME videocore
    SUFFIX ".audio"
)

# The mode file (the AUDN and AUDM chunks ahi.device reads), written by modefile.py.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(VIDEOCORE_AUDIO_MODEFILE ${CMAKE_CURRENT_BINARY_DIR}/VIDEOCORE)
add_custom_command(
    OUTPUT ${VIDEOCORE_AUDIO_MODEFILE}
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/modefile.py ${VIDEOCORE_AUDIO_MODEFILE}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/modefile.py
    COMMENT "AHI mode file VIDEOCORE"
    VERBATIM
)
add_custom_target(videocore_audio_modes ALL DEPENDS ${VIDEOCORE_AUDIO_MODEFILE})

# ahi.device opens drivers in DEVS:AHI/ and scans DEVS:AudioModes/.
install(TARGETS videocore_audio RUNTIME DESTINATION DEVS/AHI)
install(FILES ${VIDEOCORE_AUDIO_MODEFILE} DESTINATION DEVS/AudioModes)
