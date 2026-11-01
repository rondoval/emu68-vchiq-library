# videocore.codec: the MMAL client on top of vchiq.device, as a videocodec
# backend (freestanding, ROM-able).  Its function table is the backend ABI
# of videocodec/sfd/videocodec_backend.sfd; it exports no header of its own.
project(videocore.codec VERSION ${CMAKE_PROJECT_VERSION})

set(VIDEOCORE_VERSTRING "$VER: ${PROJECT_NAME} ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR} ${CURRENT_DATE}")

add_executable(videocore_codec
    src/videocore_lib.c
    src/videocore_link.c
    src/smem_client.c
    src/mmal_client.c
    src/mmal_call.c
    src/mmal_receive.c
    src/mmal_port.c
    src/mmal_buffer.c
    src/mmal_zero_copy.c
    src/mmal_wire.c
    src/videocore_session.c
    src/session_objects.c
    src/session_formats.c
    src/session_isp.c
    src/session_decoder.c
    src/h264_sps.c
    src/session_still.c
    src/session_encoder.c
    src/videocore_backend.c
    src/videocore_codecs.c
    src/videocore_image.c
    src/videocore_gencmd.c
    ${VCHIQ_SHARED_DIR}/image_info.c
)
target_compile_definitions(videocore_codec PRIVATE
    LIBRARY_IDSTRING="${VIDEOCORE_VERSTRING}"
    LIBRARY_VERSION=${PROJECT_VERSION_MAJOR}
    LIBRARY_REVISION=${PROJECT_VERSION_MINOR}
    LIBRARY_NAME="${PROJECT_NAME}"
)
target_compile_options(videocore_codec PRIVATE ${VCHIQ_MODULE_COMPILE_OPTIONS})
target_link_options(videocore_codec PRIVATE ${VCHIQ_MODULE_LINK_OPTIONS})
# vchiq.device's public header and the backend ABI only: the transport's internals are not in reach.
target_include_directories(videocore_codec PRIVATE
    include
    ${VCHIQ_SHARED_DIR}/include
)
target_link_libraries(videocore_codec PRIVATE vchiq_headers videocodec_headers Emu68Common::common)

emu68_module_layout(videocore_codec)
emu68_enable_lto(videocore_codec)

set_target_properties(videocore_codec PROPERTIES
    OUTPUT_NAME videocore
    SUFFIX ".codec"
)

# A backend lives in LIBS:videocodec/, where videocodec.library looks for it.
install(TARGETS videocore_codec RUNTIME DESTINATION LIBS/videocodec)
