# videocodec.library: the public, backend-agnostic codec API (freestanding, ROM-able).
# The backends' ABI (videocodec_backend.sfd, libraries/videocodec_backend.h) is
# generated and exported from here too, so a backend sees one copy of it.
project(videocodec.library VERSION ${CMAKE_PROJECT_VERSION})

set(VIDEOCODEC_VERSTRING "$VER: ${PROJECT_NAME} ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR} ${CURRENT_DATE}")

set(VIDEOCODEC_GENERATED_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/include)

# SFD -> clib/inline/pragmas/proto headers, for the public API and the backend ABI
emu68_sfd_headers(videocodec_sfd_headers ALL
    OUTPUT_DIR ${VIDEOCODEC_GENERATED_INCLUDE_DIR}
    SFD ${CMAKE_CURRENT_SOURCE_DIR}/sfd/videocodec.sfd ${CMAKE_CURRENT_SOURCE_DIR}/sfd/videocodec_backend.sfd
)

# The public API and the backend ABI: libraries/videocodec*.h, copied out of
# include/ so that backends and tools see nothing of the library's internals,
# and the generated headers.
set(VIDEOCODEC_PUBLIC_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/public-include)
configure_file(include/libraries/videocodec.h ${VIDEOCODEC_PUBLIC_INCLUDE_DIR}/libraries/videocodec.h COPYONLY)
configure_file(include/libraries/videocodec_backend.h ${VIDEOCODEC_PUBLIC_INCLUDE_DIR}/libraries/videocodec_backend.h COPYONLY)
add_library(videocodec_headers INTERFACE)
target_include_directories(videocodec_headers INTERFACE
    ${VIDEOCODEC_PUBLIC_INCLUDE_DIR}
    ${VIDEOCODEC_GENERATED_INCLUDE_DIR}
)
add_dependencies(videocodec_headers videocodec_sfd_headers)

add_executable(videocodec_library
    src/videocodec_lib.c
    src/videocodec_backend.c
    src/videocodec_session.c
    src/videocodec_image.c
    ${VCHIQ_SHARED_DIR}/image_info.c
)
target_compile_definitions(videocodec_library PRIVATE
    LIBRARY_IDSTRING="${VIDEOCODEC_VERSTRING}"
    LIBRARY_VERSION=${PROJECT_VERSION_MAJOR}
    LIBRARY_REVISION=${PROJECT_VERSION_MINOR}
    LIBRARY_NAME="${PROJECT_NAME}"
)
target_compile_options(videocodec_library PRIVATE ${VCHIQ_MODULE_COMPILE_OPTIONS})
target_link_options(videocodec_library PRIVATE ${VCHIQ_MODULE_LINK_OPTIONS})
# Its own headers only: no vchiq, no MMAL.
target_include_directories(videocodec_library PRIVATE
    include
    ${VIDEOCODEC_GENERATED_INCLUDE_DIR}
    ${VCHIQ_SHARED_DIR}/include
)
add_dependencies(videocodec_library videocodec_sfd_headers)
target_link_libraries(videocodec_library PRIVATE Emu68Common::common)

emu68_module_layout(videocodec_library)
emu68_enable_lto(videocodec_library)

set_target_properties(videocodec_library PROPERTIES
    OUTPUT_NAME videocodec
    SUFFIX ".library"
)

install(TARGETS videocodec_library RUNTIME DESTINATION LIBS)
install(FILES include/libraries/videocodec.h include/libraries/videocodec_backend.h DESTINATION include/libraries)
install(DIRECTORY ${VIDEOCODEC_GENERATED_INCLUDE_DIR}/ DESTINATION include)
