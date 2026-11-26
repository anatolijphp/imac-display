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
pkg_check_modules(UDEV REQUIRED IMPORTED_TARGET libudev)
pkg_check_modules(DRM REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo)
pkg_check_modules(GL REQUIRED IMPORTED_TARGET egl gbm glesv2)
pkg_check_modules(AV REQUIRED IMPORTED_TARGET libavcodec libavutil)
find_package(Threads REQUIRED)
find_package(nlohmann_json 3 REQUIRED)

add_executable(imacdisplayd
  src/main.cpp
  src/net.cpp
  src/input.cpp
  src/hidmap.cpp
  src/storage.cpp
  src/display.cpp
  src/video.cpp
)
target_link_libraries(imacdisplayd PRIVATE PkgConfig::EVDEV PkgConfig::UDEV PkgConfig::DRM PkgConfig::CAIRO PkgConfig::GL PkgConfig::AV Threads::Threads nlohmann_json::nlohmann_json)

install(TARGETS imacdisplayd RUNTIME DESTINATION bin)
