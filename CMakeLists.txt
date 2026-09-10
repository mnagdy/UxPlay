cmake_minimum_required( VERSION 3.13 )

project( uxplay )

message( STATUS "Project name: " ${PROJECT_NAME} )

include(GNUInstallDirs)

set ( CMAKE_CXX_STANDARD 11 )

if (ZOOMFIX )
  message (STATUS "cmake option ZOOMFIX is no longer used (if needed, ZOOMFIX is automatically applied if X11 libraries are present)" )
endif()

if ( ( UNIX AND NOT APPLE ) OR USE_X11 )
  if ( NOT NO_X11_DEPS )
  find_package( X11  )
    if ( X11_FOUND )
      message (STATUS "Will compile using X11 Libraries  (use cmake option -DNO_X11_DEPS=ON if X11 dependence is not wanted)" )
      link_libraries( ${X11_LIBRARIES} )
      include_directories( ${X11_INCLUDE_DIR} )
    else ()
      message (STATUS "X11 libraries not found, will compile without X11 dependence" )
    endif ()
  else()
    message (STATUS "will compile without X11 dependence" )
  endif()
endif()

if( UNIX AND NOT APPLE )
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(DBUS dbus-1>=1.4.12)
  if (DBUS_FOUND )
      add_definitions(-DDBUS )
      include_directories(${DBUS_INCLUDE_DIRS})
      #FreeBSD has this Library in /usr/local/lib
      find_library (DBUS_LIBRARY NAMES dbus-1 PATHS /usr/local/lib)
      if (NOT DBUS_LIBRARY)
      	 message(FATAL_ERROR "libdbus-1 not found")
      endif()
  endif()
endif()

if( UNIX AND NOT APPLE )
  add_definitions( -DSUPPRESS_AVAHI_COMPAT_WARNING )
  # convert AirPlay colormap 1:3:7:1 to sRGB (1:1:7:1), needed on Linux and BSD
  add_definitions( -DFULL_RANGE_RGB_FIX )
else()
  set( CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE )
endif()

add_subdirectory( lib/llhttp )
add_subdirectory( lib/playfair )
add_subdirectory( lib )
add_subdirectory( renderers )

if  ( GST_MACOS )
     add_definitions( -DGST_MACOS )
     message ( STATUS "define GST_MACOS" )
endif()

add_executable( uxplay uxplay.cpp )
target_link_libraries( uxplay renderers airplay )

option(UXPLAY_BUILD_TESTS "Build headless playback regression tests" OFF)
if(UXPLAY_BUILD_TESTS)
  enable_testing()
  add_executable(test_http_response tests/test_http_response.c lib/http_response.c)
  add_test(NAME http_response COMMAND test_http_response)
  add_executable(test_http_request tests/test_http_request.c lib/http_request.c)
  target_link_libraries(test_http_request PRIVATE llhttp)
  add_test(NAME http_request COMMAND test_http_request)
  add_executable(test_direct_playback_state tests/test_direct_playback_state.c)
  target_link_libraries(test_direct_playback_state PRIVATE m)
  add_test(NAME direct_playback_state COMMAND test_direct_playback_state)
  add_executable(test_audio_renderer tests/test_audio_renderer.c)
  target_link_libraries(test_audio_renderer PRIVATE renderers)
  add_test(NAME audio_renderer COMMAND test_audio_renderer)
  set_tests_properties(audio_renderer PROPERTIES ENVIRONMENT "G_DEBUG=fatal-warnings" TIMEOUT 30)
  add_executable(test_hls_gap_repair tests/test_hls_gap_repair.c)
  target_link_libraries(test_hls_gap_repair PRIVATE renderers)
  add_test(NAME hls_gap_repair COMMAND test_hls_gap_repair)
  add_executable(test_direct_renderer tests/test_direct_renderer.c)
  target_link_libraries(test_direct_renderer PRIVATE renderers)
  add_test(NAME direct_renderer COMMAND test_direct_renderer)
  add_executable(test_playback_diagnostics tests/test_playback_diagnostics.c)
  target_link_libraries(test_playback_diagnostics PRIVATE renderers)
  add_test(NAME playback_diagnostics COMMAND test_playback_diagnostics)
  add_executable(test_airplay_video_cache tests/test_airplay_video_cache.c)
  target_link_libraries(test_airplay_video_cache PRIVATE airplay)
  add_test(NAME airplay_video_cache COMMAND test_airplay_video_cache)
  if(UNIX)
    add_executable(test_http_hls_cache tests/test_http_hls_cache.c)
    target_link_libraries(test_http_hls_cache PRIVATE airplay playfair m)
    # This test includes raop.c to exercise its private HTTP handlers. Match
    # that source's platform/libplist definitions and dependency headers.
    get_directory_property(UXPLAY_LIB_DEFINITIONS DIRECTORY lib COMPILE_DEFINITIONS)
    target_compile_definitions(test_http_hls_cache PRIVATE ${UXPLAY_LIB_DEFINITIONS})
    target_include_directories(test_http_hls_cache PRIVATE $<TARGET_PROPERTY:airplay,INCLUDE_DIRECTORIES>)
    add_test(NAME http_hls_cache COMMAND test_http_hls_cache)
    set_tests_properties(http_hls_cache PROPERTIES TIMEOUT 15)
  endif()
  add_executable(test_direct_renderer_hls tests/test_direct_renderer_hls.c)
  target_link_libraries(test_direct_renderer_hls PRIVATE renderers)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME http_response_keepalive COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_http_response_test.py $<TARGET_FILE:test_http_response>)
    set_tests_properties(http_response_keepalive PROPERTIES TIMEOUT 10)
    add_test(NAME playback_diagnostics_http COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_diagnostics_http_test.py $<TARGET_FILE:test_playback_diagnostics>)
    set_tests_properties(playback_diagnostics_http PROPERTIES
      ENVIRONMENT "G_DEBUG=fatal-warnings" TIMEOUT 45)
  endif()
  find_program(FFMPEG_EXECUTABLE ffmpeg)
  if(Python3_Interpreter_FOUND AND FFMPEG_EXECUTABLE)
    add_test(NAME direct_renderer_hls_gaps COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_hls_gap_test.py $<TARGET_FILE:test_direct_renderer_hls>)
    set_tests_properties(direct_renderer_hls_gaps PROPERTIES
      ENVIRONMENT "G_DEBUG=fatal-warnings" TIMEOUT 180)
    add_test(NAME direct_renderer_http_hls COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_hls_renderer_test.py $<TARGET_FILE:test_direct_renderer_hls>)
    set_tests_properties(direct_renderer_http_hls PROPERTIES
      ENVIRONMENT "G_DEBUG=fatal-warnings" TIMEOUT 150)
  endif()
  set_tests_properties(direct_renderer playback_diagnostics PROPERTIES
    ENVIRONMENT "G_DEBUG=fatal-warnings" TIMEOUT 45)
endif()
if (DBUS_FOUND)
      target_link_libraries( uxplay ${DBUS_LIBRARY})
endif()

install( TARGETS  uxplay RUNTIME DESTINATION bin )
install( FILES uxplay.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1 )
install( FILES README.md README.txt README.html LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR} ) 
install( FILES lib/llhttp/LICENSE-MIT DESTINATION ${CMAKE_INSTALL_DOCDIR}/llhttp )
install( FILES uxplay.service DESTINATION ${CMAKE_INSTALL_DOCDIR}/systemd ) 
install( FILES Bluetooth_LE_beacon/uxplay-beacon.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1 )
install( FILES Bluetooth_LE_beacon/uxplay-beacon.py
         DESTINATION bin
         PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ GROUP_EXECUTE GROUP_READ  WORLD_EXECUTE WORLD_READ)
install( FILES Bluetooth_LE_beacon/uxplay_beacon_module_BleuIO.py
         DESTINATION bin
         PERMISSIONS  OWNER_WRITE OWNER_READ GROUP_READ WORLD_READ)

if (DBUS_FOUND)
install( FILES Bluetooth_LE_beacon/uxplay_beacon_module_BlueZ.py
         DESTINATION bin
         PERMISSIONS OWNER_WRITE OWNER_READ GROUP_READ WORLD_READ)
endif()

if (WIN32)
install( FILES Bluetooth_LE_beacon/uxplay_beacon_module_winrt.py
         DESTINATION bin
         PERMISSIONS OWNER_WRITE OWNER_READ GROUP_READ WORLD_READ)
endif()

if (UNIX AND NOT APPLE)
install( FILES Bluetooth_LE_beacon/uxplay_beacon_module_HCI.py
         DESTINATION bin
         PERMISSIONS OWNER_WRITE OWNER_READ GROUP_READ WORLD_READ)
endif()


# uninstall target
if(NOT TARGET uninstall)
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake_uninstall.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    IMMEDIATE @ONLY)

  add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)
endif()
