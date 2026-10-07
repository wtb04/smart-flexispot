# Install script for directory: /Users/wtb/dev/tab5-hello-design/managed_components/espressif__libjpeg-turbo/libjpeg-turbo

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/Users/wtb/dev/tab5-hello-design/build_kit/esp-idf/espressif__libjpeg-turbo/libjpeg-build/install")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "TRUE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/Users/wtb/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20260121/riscv32-esp-elf/bin/riscv32-esp-elf-objdump")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "lib" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "/Users/wtb/dev/tab5-hello-design/build_kit/esp-idf/espressif__libjpeg-turbo/libjpeg-build/libjpeg.a")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "doc" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/doc/libjpeg-turbo" TYPE FILE FILES
    "/Users/wtb/dev/tab5-hello-design/managed_components/espressif__libjpeg-turbo/libjpeg-turbo/README.ijg"
    "/Users/wtb/dev/tab5-hello-design/managed_components/espressif__libjpeg-turbo/libjpeg-turbo/README.md"
    "/Users/wtb/dev/tab5-hello-design/managed_components/espressif__libjpeg-turbo/libjpeg-turbo/src/example.c"
    "/Users/wtb/dev/tab5-hello-design/managed_components/espressif__libjpeg-turbo/libjpeg-turbo/src/tjcomp.c"
    "/Users/wtb/dev/tab5-hello-design/managed_components/espressif__libjpeg-turbo/libjpeg-turbo/src/tjdecomp.c"
    "/Users/wtb/dev/tab5-hello-design/managed_components/espressif__libjpeg-turbo/libjpeg-turbo/src/tjtran.c"
    "/Users/wtb/dev/tab5-hello-design/managed_components/espressif__libjpeg-turbo/libjpeg-turbo/doc/libjpeg.txt"
    "/Users/wtb/dev/tab5-hello-design/managed_components/espressif__libjpeg-turbo/libjpeg-turbo/doc/structure.txt"
    "/Users/wtb/dev/tab5-hello-design/managed_components/espressif__libjpeg-turbo/libjpeg-turbo/doc/usage.txt"
    "/Users/wtb/dev/tab5-hello-design/managed_components/espressif__libjpeg-turbo/libjpeg-turbo/doc/wizard.txt"
    "/Users/wtb/dev/tab5-hello-design/managed_components/espressif__libjpeg-turbo/libjpeg-turbo/LICENSE.md"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "lib" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/pkgconfig" TYPE FILE FILES "/Users/wtb/dev/tab5-hello-design/build_kit/esp-idf/espressif__libjpeg-turbo/libjpeg-build/pkgscripts/libjpeg.pc")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "lib" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/libjpeg-turbo" TYPE FILE FILES
    "/Users/wtb/dev/tab5-hello-design/build_kit/esp-idf/espressif__libjpeg-turbo/libjpeg-build/pkgscripts/libjpeg-turboConfig.cmake"
    "/Users/wtb/dev/tab5-hello-design/build_kit/esp-idf/espressif__libjpeg-turbo/libjpeg-build/pkgscripts/libjpeg-turboConfigVersion.cmake"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "lib" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/libjpeg-turbo/libjpeg-turboTargets.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/libjpeg-turbo/libjpeg-turboTargets.cmake"
         "/Users/wtb/dev/tab5-hello-design/build_kit/esp-idf/espressif__libjpeg-turbo/libjpeg-build/CMakeFiles/Export/f0d506f335508d6549928070f26fb787/libjpeg-turboTargets.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/libjpeg-turbo/libjpeg-turboTargets-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/libjpeg-turbo/libjpeg-turboTargets.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/libjpeg-turbo" TYPE FILE FILES "/Users/wtb/dev/tab5-hello-design/build_kit/esp-idf/espressif__libjpeg-turbo/libjpeg-build/CMakeFiles/Export/f0d506f335508d6549928070f26fb787/libjpeg-turboTargets.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/libjpeg-turbo" TYPE FILE FILES "/Users/wtb/dev/tab5-hello-design/build_kit/esp-idf/espressif__libjpeg-turbo/libjpeg-build/CMakeFiles/Export/f0d506f335508d6549928070f26fb787/libjpeg-turboTargets-release.cmake")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "include" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include" TYPE FILE FILES
    "/Users/wtb/dev/tab5-hello-design/build_kit/esp-idf/espressif__libjpeg-turbo/libjpeg-build/jconfig.h"
    "/Users/wtb/dev/tab5-hello-design/managed_components/espressif__libjpeg-turbo/libjpeg-turbo/src/jerror.h"
    "/Users/wtb/dev/tab5-hello-design/managed_components/espressif__libjpeg-turbo/libjpeg-turbo/src/jmorecfg.h"
    "/Users/wtb/dev/tab5-hello-design/managed_components/espressif__libjpeg-turbo/libjpeg-turbo/src/jpeglib.h"
    )
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/wtb/dev/tab5-hello-design/build_kit/esp-idf/espressif__libjpeg-turbo/libjpeg-build/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
if(CMAKE_INSTALL_COMPONENT)
  if(CMAKE_INSTALL_COMPONENT MATCHES "^[a-zA-Z0-9_.+-]+$")
    set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INSTALL_COMPONENT}.txt")
  else()
    string(MD5 CMAKE_INST_COMP_HASH "${CMAKE_INSTALL_COMPONENT}")
    set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INST_COMP_HASH}.txt")
    unset(CMAKE_INST_COMP_HASH)
  endif()
else()
  set(CMAKE_INSTALL_MANIFEST "install_manifest.txt")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/wtb/dev/tab5-hello-design/build_kit/esp-idf/espressif__libjpeg-turbo/libjpeg-build/${CMAKE_INSTALL_MANIFEST}"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
