cmake_minimum_required(VERSION 3.16)

Project(SettingsStorageLib)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# set(CMAKE_VERBOSE_MAKEFILE ON)

# Include the subdirectories
add_subdirectory(Source)
add_subdirectory(Tests)
