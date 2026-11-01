# vcjpeg.datatype: a picture.datatype subclass that decodes JPEGs through
# videocodec.library (freestanding), and its descriptor VCJPEG, whose
# recognition function (DTCD) decides which JPEGs come to it.  The descriptor
# is added after the system's (docs/architecture.md section 8): it installs to
# Storage/DataTypes, and the Install script adds it from S:User-Startup.
project(vcjpeg.datatype VERSION ${CMAKE_PROJECT_VERSION})

set(VCJPEG_VERSTRING "$VER: ${PROJECT_NAME} ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR} ${CURRENT_DATE}")

add_executable(vcjpeg_datatype
    src/vcjpeg_lib.c
    src/vcjpeg_class.c
    ${VCHIQ_SHARED_DIR}/image_info.c
)
target_compile_definitions(vcjpeg_datatype PRIVATE
    LIBRARY_IDSTRING="${VCJPEG_VERSTRING}"
    LIBRARY_VERSION=${PROJECT_VERSION_MAJOR}
    LIBRARY_REVISION=${PROJECT_VERSION_MINOR}
    LIBRARY_NAME="${PROJECT_NAME}"
)
target_compile_options(vcjpeg_datatype PRIVATE ${VCHIQ_MODULE_COMPILE_OPTIONS})
target_link_options(vcjpeg_datatype PRIVATE ${VCHIQ_MODULE_LINK_OPTIONS})
# videocodec.library's public API and the shared header reader only.
target_include_directories(vcjpeg_datatype PRIVATE
    include
    ${VCHIQ_SHARED_DIR}/include
)
target_link_libraries(vcjpeg_datatype PRIVATE videocodec_headers Emu68Common::common)

emu68_module_layout(vcjpeg_datatype)
emu68_enable_lto(vcjpeg_datatype)

set_target_properties(vcjpeg_datatype PROPERTIES
    OUTPUT_NAME vcjpeg
    SUFFIX ".datatype"
)

# The recognition function: a hunk file of its own whose first word is its
# entry, no data.  The module layout places .text.entry first; its
# do-not-execute check does not apply (this is called, not run), and -e names
# the entry in place of the script's.
add_executable(vcjpeg_recog src/vcjpeg_recog.c)
target_compile_definitions(vcjpeg_recog PRIVATE LIBRARY_NAME="${PROJECT_NAME}")
target_compile_options(vcjpeg_recog PRIVATE ${VCHIQ_MODULE_COMPILE_OPTIONS})
target_link_options(vcjpeg_recog PRIVATE ${VCHIQ_MODULE_LINK_OPTIONS} "-Wl,-T,${_EMU68_MODULE_LDSCRIPT}" "-Wl,-e,_vcjpeg_recognise")
set_property(TARGET vcjpeg_recog APPEND PROPERTY LINK_DEPENDS "${_EMU68_MODULE_LDSCRIPT}")
target_include_directories(vcjpeg_recog PRIVATE include ${VCHIQ_SHARED_DIR}/include) # jpeg.h
target_link_libraries(vcjpeg_recog PRIVATE Emu68Common::common)

# The descriptor, with the function in it, parsed back by its generator before it is kept.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(VCJPEG_DESCRIPTOR ${CMAKE_CURRENT_BINARY_DIR}/VCJPEG)
add_custom_command(
    OUTPUT ${VCJPEG_DESCRIPTOR}
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/descriptor.py ${VCJPEG_DESCRIPTOR} $<TARGET_FILE:vcjpeg_recog>
            "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}" "${CURRENT_DATE}"
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/descriptor.py vcjpeg_recog
    COMMENT "DataTypes descriptor VCJPEG"
    VERBATIM
)
add_custom_target(vcjpeg_descriptor ALL DEPENDS ${VCJPEG_DESCRIPTOR})

# datatypes.library opens classes as datatypes/<name>.datatype through LIBS:, which takes in SYS:Classes.
install(TARGETS vcjpeg_datatype RUNTIME DESTINATION Classes/DataTypes)
install(FILES ${VCJPEG_DESCRIPTOR} DESTINATION Storage/DataTypes)
