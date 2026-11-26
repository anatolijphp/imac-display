cmake_minimum_required(VERSION 3.20)
project(imacdisplayd CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

find_package(PkgConfig REQUIRED)
pkg_check_modules(EVDEV REQUIRED IMPORTED_TARGET libevdev)
find_package(nlohmann_json 3 REQUIRED)

add_executable(imacdisplayd
  src/main.cpp
  src/net.cpp
  src/input.cpp
  src/hidmap.cpp
)
target_link_libraries(imacdisplayd PRIVATE PkgConfig::EVDEV nlohmann_json::nlohmann_json)

install(TARGETS imacdisplayd RUNTIME DESTINATION bin)
