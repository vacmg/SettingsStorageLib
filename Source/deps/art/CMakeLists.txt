add_library(libartcpp STATIC)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(
        libart
        GIT_REPOSITORY  https://github.com/armon/libart.git
        GIT_TAG         master
)
FetchContent_MakeAvailable(libart)

FetchContent_GetProperties(libart)

file(GLOB libart_SOURCES "${libart_SOURCE_DIR}/src/*.c")

target_sources(libartcpp PRIVATE "${libart_SOURCES}")
target_compile_options(libartcpp PUBLIC -Wno-pedantic)
target_include_directories(libartcpp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${libart_SOURCE_DIR}/src")
