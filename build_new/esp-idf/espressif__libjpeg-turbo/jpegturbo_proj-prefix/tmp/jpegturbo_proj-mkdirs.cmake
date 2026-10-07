# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/wtb/dev/tab5-hello-design/managed_components/espressif__libjpeg-turbo/libjpeg-turbo")
  file(MAKE_DIRECTORY "/Users/wtb/dev/tab5-hello-design/managed_components/espressif__libjpeg-turbo/libjpeg-turbo")
endif()
file(MAKE_DIRECTORY
  "/Users/wtb/dev/tab5-hello-design/build_new/esp-idf/espressif__libjpeg-turbo/libjpeg-build"
  "/Users/wtb/dev/tab5-hello-design/build_new/esp-idf/espressif__libjpeg-turbo/jpegturbo_proj-prefix"
  "/Users/wtb/dev/tab5-hello-design/build_new/esp-idf/espressif__libjpeg-turbo/jpegturbo_proj-prefix/tmp"
  "/Users/wtb/dev/tab5-hello-design/build_new/esp-idf/espressif__libjpeg-turbo/jpegturbo_proj-prefix/src/jpegturbo_proj-stamp"
  "/Users/wtb/dev/tab5-hello-design/build_new/esp-idf/espressif__libjpeg-turbo/jpegturbo_proj-prefix/src"
  "/Users/wtb/dev/tab5-hello-design/build_new/esp-idf/espressif__libjpeg-turbo/jpegturbo_proj-prefix/src/jpegturbo_proj-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/wtb/dev/tab5-hello-design/build_new/esp-idf/espressif__libjpeg-turbo/jpegturbo_proj-prefix/src/jpegturbo_proj-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/wtb/dev/tab5-hello-design/build_new/esp-idf/espressif__libjpeg-turbo/jpegturbo_proj-prefix/src/jpegturbo_proj-stamp${cfgdir}") # cfgdir has leading slash
endif()
