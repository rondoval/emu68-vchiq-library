# swconv.codec: a software converter backend for the test suite only (tests/test_choice.py).
# Built with the rest, never installed: the suite takes it from the build tree.
project(swconv.codec VERSION ${CMAKE_PROJECT_VERSION})

set(SWCONV_VERSTRING "$VER: ${PROJECT_NAME} ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR} ${CURRENT_DATE} test only")

add_executable(swconv_codec swconv.c)
target_compile_definitions(swconv_codec PRIVATE
    LIBRARY_IDSTRING="${SWCONV_VERSTRING}"
    LIBRARY_VERSION=${PROJECT_VERSION_MAJOR}
    LIBRARY_REVISION=${PROJECT_VERSION_MINOR}
    LIBRARY_NAME="${PROJECT_NAME}"
)
target_compile_options(swconv_codec PRIVATE ${VCHIQ_MODULE_COMPILE_OPTIONS})
target_link_options(swconv_codec PRIVATE ${VCHIQ_MODULE_LINK_OPTIONS})
target_link_libraries(swconv_codec PRIVATE videocodec_headers Emu68Common::common)

emu68_module_layout(swconv_codec)
emu68_enable_lto(swconv_codec)

set_target_properties(swconv_codec PROPERTIES
    OUTPUT_NAME swconv
    SUFFIX ".codec"
)
