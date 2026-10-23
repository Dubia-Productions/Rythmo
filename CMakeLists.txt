cmake_minimum_required(VERSION 3.25)
set(RYTHMO_VERSION "0.3.0" CACHE STRING "Rythmo application version")
project(rythmo VERSION "${RYTHMO_VERSION}" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

include(GNUInstallDirs)
include(CTest)
find_package(EXPAT REQUIRED)
find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Multimedia MultimediaWidgets Network)

add_library(rythmo src/detx.cpp)
target_include_directories(rythmo PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(rythmo PRIVATE EXPAT::EXPAT)
target_compile_features(rythmo PUBLIC cxx_std_17)

add_executable(rythmo-cli src/main.cpp)
target_link_libraries(rythmo-cli PRIVATE rythmo)

add_executable(rythmo-app
  src/gui_main.cpp
  src/main_window.cpp
  src/rhythm_timeline.cpp
  resources/rythmo_fonts.qrc
)
if(DEFINED RYTHMO_PYTHON_RUNTIME_ARCHIVE AND EXISTS "${RYTHMO_PYTHON_RUNTIME_ARCHIVE}")
  get_filename_component(RYTHMO_PYTHON_RUNTIME_ARCHIVE "${RYTHMO_PYTHON_RUNTIME_ARCHIVE}" ABSOLUTE)
  configure_file(resources/python_runtime.qrc.in
                 "${CMAKE_CURRENT_BINARY_DIR}/python_runtime.qrc" @ONLY)
  target_sources(rythmo-app PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/python_runtime.qrc")
  message(STATUS "Bundled native Python runtime: ${RYTHMO_PYTHON_RUNTIME_ARCHIVE}")
else()
  message(WARNING "No native Python runtime archive configured; voice separation will require a bundled runtime at package time.")
endif()
set_target_properties(rythmo-app PROPERTIES OUTPUT_NAME Rythmo)
target_include_directories(rythmo-app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(rythmo-app PRIVATE rythmo Qt6::Widgets Qt6::Multimedia Qt6::MultimediaWidgets Qt6::Network)
target_compile_definitions(rythmo-app PRIVATE RYTHMO_VERSION="${PROJECT_VERSION}")

if(WIN32)
  set_target_properties(rythmo-app PROPERTIES WIN32_EXECUTABLE TRUE)
  # The Windows x64 build script can target the portable package directly;
  # keep intermediate objects and tests in the regular build tree.
  if(DEFINED RYTHMO_DISTRIBUTION_DIRECTORY AND NOT RYTHMO_DISTRIBUTION_DIRECTORY STREQUAL "")
    set_target_properties(rythmo-app PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY_RELEASE "${RYTHMO_DISTRIBUTION_DIRECTORY}"
    )
  endif()
endif()
if(APPLE)
  # The plist names this icon; include the ICNS in Contents/Resources so
  # Finder and Launch Services can load it from the application bundle.
  set_source_files_properties(resources/Rythmo.icns PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
  target_sources(rythmo-app PRIVATE resources/Rythmo.icns)
  set_target_properties(rythmo-app PROPERTIES
    MACOSX_BUNDLE_ICON_FILE "Rythmo.icns"
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.dubiaproductions.rythmo"
    MACOSX_BUNDLE_BUNDLE_NAME "Rythmo"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_LONG_VERSION_STRING "Rythmo ${PROJECT_VERSION}"
    MACOSX_BUNDLE_COPYRIGHT "Copyright Dubia Productions"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/resources/macos/Info.plist.in"
  )
endif()

if(BUILD_TESTING)
  add_executable(rythmo_tests tests/test_detx.cpp)
  target_link_libraries(rythmo_tests PRIVATE rythmo)
  add_test(NAME rythmo_tests COMMAND rythmo_tests)
  add_executable(rythmo_export_tests tests/test_video_export.cpp)
  target_include_directories(rythmo_export_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(rythmo_export_tests PRIVATE Qt6::Gui)
  if(MSVC)
    target_compile_options(rythmo_export_tests PRIVATE /utf-8)
  endif()
  add_test(NAME rythmo_export_tests COMMAND rythmo_export_tests)
  set_tests_properties(rythmo_export_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)

endif()

foreach(target rythmo rythmo-cli rythmo-app)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endforeach()
if(TARGET rythmo_tests)
  if(MSVC)
    target_compile_options(rythmo_tests PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(rythmo_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()

install(TARGETS rythmo-app rythmo-cli RUNTIME DESTINATION . BUNDLE DESTINATION .)
