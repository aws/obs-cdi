# --------------------------------------------------------------------------------
# Note: Must define the following variables on the cmake command line:
#
# CDI_DIR= Path to CDI-SDK root folder.
# CMAKE_INSTALL_PREFIX= Path where the built OBS Studio is installed.
#
# For example, for Windows:
#  -DCDI_DIR="C:/CDI/aws-cdi-sdk"
#  -DCMAKE_INSTALL_PREFIX="C:/obs-studio"
#
# For example, for Linux:
#  -DCDI_DIR="/home/rocky/CDI/aws-cdi-sdk"
#  -DCMAKE_INSTALL_PREFIX="/home/rocky/obs-studio-portable"
# --------------------------------------------------------------------------------

set (OBS_DIR "${CMAKE_INSTALL_PREFIX}")

# Should not have to modify these settings, but here incase OBS changes them.
set (OBS_BUILDDIR_ARCH "build_x64")
set (OBS_ARCH_NAME "64bit")
set (libobs_DIR "${OBS_DIR}/cmake/libobs")
set (obs-frontend-api_DIR "${OBS_DIR}/cmake/obs-frontend-api")

# --------------------------------------------------------------------------------
# Unless otherwise stated "For CDI-plugin", all lines below here are from the OBS
# plug-in temmplate and should not require any changes.
# --------------------------------------------------------------------------------

# For CDI-plugin.
set (ENABLE_FRONTEND_API "true")
set (ENABLE_QT "true")

cmake_minimum_required(VERSION 3.16...3.26)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_qt(COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt::Core Qt::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME} PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header
                                  -Wno-comma>)
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON
               AUTOUIC ON
               AUTORCC ON)
endif()

# For CDI-plugin. Add CDI includes and link library.
include_directories("${CDI_DIR}/include")
if(WIN32)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE "${CDI_DIR}/proj/x64/$<CONFIG>_DLL/cdi_sdk.lib")
else()
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(BUILD_TYPE "debug")
  else()
    set(BUILD_TYPE "release")
  endif()

  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE "${CDI_DIR}/build/${BUILD_TYPE}/lib/libcdisdk.so")
  add_compile_definitions(_LINUX)
endif()

# For CDI-plugin. Add source and header files.
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
	src/Config.cpp
	src/obs-cdi.cpp
	src/obs-cdi-source.cpp
	src/obs-cdi-output.cpp
	src/main-output.cpp
	src/output-settings.cpp

    PRIVATE FILE_SET HEADERS FILES
	src/Config.h
	src/obs-cdi.h
	src/main-output.h
	src/output-settings.h)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# For CDI-plugin. Windows post build commands.
if(WIN32)
	set(CDI_LIB_DIR "${CDI_DIR}//proj/x64/$<CONFIG>_DLL")
	set(DEST_ROOT_DIR "${OBS_DIR}/${OBS_BUILDDIR_ARCH}/rundir/$<CONFIG>")
	set(DEST_BIN_DIR "${DEST_ROOT_DIR}/bin/${OBS_ARCH_NAME}")
	add_custom_command(TARGET obs-cdi POST_BUILD

			COMMAND  ("${CMAKE_COMMAND}" -E copy
				"$<TARGET_FILE:obs-cdi>"
				"${DEST_ROOT_DIR}/obs-plugins/${OBS_ARCH_NAME}"
			)
			COMMAND  ("${CMAKE_COMMAND}" -E copy
				"${CDI_LIB_DIR}/cdi_sdk.dll"
				"${DEST_BIN_DIR}"
			)
			COMMAND  ("${CMAKE_COMMAND}" -E copy
				"${CDI_LIB_DIR}/cdi_sdk.pdb"
				"${DEST_BIN_DIR}"
			)
			COMMAND  ("${CMAKE_COMMAND}" -E copy
				"${CDI_LIB_DIR}/efawin.dll"
				"${DEST_BIN_DIR}"
			)
			COMMAND  ("${CMAKE_COMMAND}" -E copy
				"${CDI_LIB_DIR}/libfabric.dll"
				"${DEST_BIN_DIR}"
			)
			COMMAND  ("${CMAKE_COMMAND}" -E copy
				"${CDI_LIB_DIR}/libfabric.pdb"
				"${DEST_BIN_DIR}"
			)
			COMMAND  ("${CMAKE_COMMAND}" -E copy
				"${CDI_LIB_DIR}/libfabric_new.dll"
				"${DEST_BIN_DIR}"
			)
			COMMAND  ("${CMAKE_COMMAND}" -E copy
				"${CDI_LIB_DIR}/libfabric_new.pdb"
				"${DEST_BIN_DIR}"
			)
			COMMAND  ("${CMAKE_COMMAND}" -E copy
				"${CDI_LIB_DIR}/cdi_libfabric_api.dll"
				"${DEST_BIN_DIR}"
			)
			COMMAND  ("${CMAKE_COMMAND}" -E copy
				"${CDI_LIB_DIR}/cdi_libfabric_api.pdb"
				"${DEST_BIN_DIR}"
			)
			COMMAND  ("${CMAKE_COMMAND}" -E copy
				"${CDI_LIB_DIR}/cdi_libfabric_new_api.dll"
				"${DEST_BIN_DIR}"
			)
			COMMAND  ("${CMAKE_COMMAND}" -E copy
				"${CDI_LIB_DIR}/cdi_libfabric_new_api.pdb"
				"${DEST_BIN_DIR}"
			)
			COMMAND  ("${CMAKE_COMMAND}" -E make_directory
				"${DEST_ROOT_DIR}/data/obs-plugins/obs-cdi"
			)
			COMMAND  ("${CMAKE_COMMAND}" -E make_directory
				"${DEST_ROOT_DIR}/data/obs-plugins/obs-cdi/locale"
			)
			COMMAND  ("${CMAKE_COMMAND}" -E copy_directory
				"${PROJECT_SOURCE_DIR}/locale"
				"${DEST_ROOT_DIR}/data/obs-plugins/obs-cdi/locale"
			)
	)
else()
	set(CDI_LIB_DIR "${CDI_DIR}/build/${BUILD_TYPE}/lib")
	set(DEST_ROOT_DIR "${OBS_DIR}")
	set(DEST_BIN_DIR "${OBS_DIR}/bin/${OBS_ARCH_NAME}")

	install(DIRECTORY "${CDI_LIB_DIR}/" DESTINATION "${DEST_BIN_DIR}")
	install(FILES "build/${CMAKE_PROJECT_NAME}.so" DESTINATION "${DEST_ROOT_DIR}/obs-plugins/${OBS_ARCH_NAME}")

	install(DIRECTORY DESTINATION "${DEST_ROOT_DIR}/data/obs-plugins/obs-cdi")
	install(DIRECTORY DESTINATION "${DEST_ROOT_DIR}/data/obs-plugins/obs-cdi/locale")
	install(DIRECTORY "${PROJECT_SOURCE_DIR}/locale/" DESTINATION "${DEST_ROOT_DIR}/data/obs-plugins/obs-cdi/locale")
endif()
