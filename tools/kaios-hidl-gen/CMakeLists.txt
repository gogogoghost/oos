cmake_minimum_required(VERSION 3.16)
project(kaios_hidl_gen_host LANGUAGES CXX)

set(KAIOS_HIDL_GEN_SOURCE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../third_party/kaios-hidl-gen"
    CACHE PATH "Path to the unmodified KaiOS hidl-gen checkout")

if(NOT EXISTS "${KAIOS_HIDL_GEN_SOURCE_DIR}/Android.bp")
    message(FATAL_ERROR "KaiOS hidl-gen source directory not found: ${KAIOS_HIDL_GEN_SOURCE_DIR}")
endif()

find_package(BISON REQUIRED)
find_package(FLEX REQUIRED)
find_package(OpenSSL REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Bison 3.8 rejects two obsolete directives in the 2019 grammar. Work on a
# build-tree copy so the checked-out KaiOS source remains byte-for-byte intact.
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(COMPAT_GRAMMAR "${GENERATED_DIR}/hidl-gen_y.yy")
file(MAKE_DIRECTORY "${GENERATED_DIR}")
add_custom_command(
    OUTPUT "${COMPAT_GRAMMAR}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${GENERATED_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "${KAIOS_HIDL_GEN_SOURCE_DIR}/hidl-gen_y.yy" "${COMPAT_GRAMMAR}"
    COMMAND "${BISON_EXECUTABLE}" --update "${COMPAT_GRAMMAR}"
    COMMAND "${CMAKE_COMMAND}" "-DGRAMMAR=${COMPAT_GRAMMAR}"
            -P "${CMAKE_CURRENT_LIST_DIR}/remove_glr_pure.cmake"
    DEPENDS "${KAIOS_HIDL_GEN_SOURCE_DIR}/hidl-gen_y.yy"
    VERBATIM)

BISON_TARGET(hidl_parser "${COMPAT_GRAMMAR}" "${GENERATED_DIR}/hidl-gen_y.cpp"
             DEFINES_FILE "${GENERATED_DIR}/hidl-gen_y.h")
FLEX_TARGET(hidl_lexer "${KAIOS_HIDL_GEN_SOURCE_DIR}/hidl-gen_l.ll"
            "${GENERATED_DIR}/hidl-gen_l.cpp")
ADD_FLEX_BISON_DEPENDENCY(hidl_lexer hidl_parser)
set_source_files_properties(${FLEX_hidl_lexer_OUTPUTS} PROPERTIES
    COMPILE_OPTIONS "-include;${CMAKE_CURRENT_LIST_DIR}/lexer_compat.hpp")

set(HIDL_GEN_SOURCES
    Annotation.cpp ArrayType.cpp CompoundType.cpp ConstantExpression.cpp
    DeathRecipientType.cpp DocComment.cpp EnumType.cpp HandleType.cpp
    HidlTypeAssertion.cpp Interface.cpp Location.cpp MemoryType.cpp Method.cpp
    NamedType.cpp PointerType.cpp FmqType.cpp RefType.cpp ScalarType.cpp Scope.cpp
    StringType.cpp Type.cpp TypeDef.cpp VectorType.cpp
    Coordinator.cpp generateCpp.cpp generateCppAdapter.cpp generateCppImpl.cpp
    generateDependencies.cpp generateJava.cpp generateVts.cpp AST.cpp main.cpp
    hashing/Hash.cpp host_utils/Formatter.cpp host_utils/StringHelper.cpp
    utils/FQName.cpp utils/FqInstance.cpp)
list(TRANSFORM HIDL_GEN_SOURCES PREPEND "${KAIOS_HIDL_GEN_SOURCE_DIR}/")
list(APPEND HIDL_GEN_SOURCES ${BISON_hidl_parser_OUTPUTS} ${FLEX_hidl_lexer_OUTPUTS})

find_library(ANDROID_BASE_LIBRARY base HINTS /usr/lib/x86_64-linux-gnu/android REQUIRED)
find_library(ANDROID_UTILS_LIBRARY utils HINTS /usr/lib/x86_64-linux-gnu/android REQUIRED)
find_library(ANDROID_LOG_LIBRARY log HINTS /usr/lib/x86_64-linux-gnu/android REQUIRED)

add_executable(hidl-gen ${HIDL_GEN_SOURCES})
target_include_directories(hidl-gen PRIVATE
    "${KAIOS_HIDL_GEN_SOURCE_DIR}"
    "${KAIOS_HIDL_GEN_SOURCE_DIR}/hashing/include"
    "${KAIOS_HIDL_GEN_SOURCE_DIR}/host_utils/include"
    "${KAIOS_HIDL_GEN_SOURCE_DIR}/host_utils/include/hidl-util"
    "${KAIOS_HIDL_GEN_SOURCE_DIR}/utils/include"
    "${KAIOS_HIDL_GEN_SOURCE_DIR}/utils/include/hidl-util"
    "${GENERATED_DIR}"
    /usr/include/android)
target_compile_options(hidl-gen PRIVATE -Wall -Wextra -include
                       "${CMAKE_CURRENT_LIST_DIR}/compat.hpp")
target_link_libraries(hidl-gen PRIVATE
    "${ANDROID_BASE_LIBRARY}" "${ANDROID_UTILS_LIBRARY}" "${ANDROID_LOG_LIBRARY}"
    OpenSSL::SSL OpenSSL::Crypto)
set_target_properties(hidl-gen PROPERTIES
    BUILD_RPATH "/usr/lib/x86_64-linux-gnu/android")
