include_directories(${DARKTABLE_BINDIR})

find_package(PkgConfig REQUIRED)
pkg_check_modules(JSON_GLIB REQUIRED json-glib-1.0)

add_executable(darktable-api main.c)

set_target_properties(darktable-api PROPERTIES LINKER_LANGUAGE C)

target_include_directories(darktable-api PRIVATE ${JSON_GLIB_INCLUDE_DIRS})
target_link_directories(darktable-api PRIVATE ${JSON_GLIB_LIBRARY_DIRS})
target_link_libraries(darktable-api lib_darktable whereami ${JSON_GLIB_LIBRARIES})

if(NOT WIN32)
  set_target_properties(darktable-api
                        PROPERTIES
                        INSTALL_RPATH ${RPATH_ORIGIN}/${REL_BIN_TO_LIBDIR}
                        RUNTIME_OUTPUT_DIRECTORY ${DARKTABLE_BINDIR})
endif()

install(TARGETS darktable-api DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DTApplication)
