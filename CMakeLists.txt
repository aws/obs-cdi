cmake_minimum_required(VERSION 3.5)
project(obs-cdi VERSION 1.6.0)

set(CMAKE_PREFIX_PATH "${QTDIR}")
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (${CMAKE_SYSTEM_PROCESSOR} MATCHES "arm") 
    set(CMAKE_CXX_FLAGS "-mfpu=neon")
endif()

if (WIN32 OR APPLE)
    include("${LIBOBS_INCLUDE_DIR}/../cmake/external/FindLibobs.cmake")
endif()

find_package(LibObs REQUIRED)
find_package(Qt5 REQUIRED Core Widgets)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
	set(ARCH 64)
else()
	set(ARCH 32)
endif()

set(obs-cdi_SOURCES
	src/obs-cdi.cpp
	src/obs-cdi-output.cpp
	src/main-output.cpp
	src/Config.cpp
	src/output-settings.cpp)

set(obs-cdi_HEADERS
	src/obs-cdi.h
	src/main-output.h
	src/Config.h
	src/output-settings.h)

add_library(obs-cdi MODULE
	${obs-cdi_SOURCES}
	${obs-cdi_HEADERS})

include_directories(
	"${LIBOBS_INCLUDE_DIR}/../UI/obs-frontend-api"
	"${CDI_INCLUDE_DIR}"
	"${CDI_LIB_DIR}"
	"${LIBFABRIC_INCLUDE_DIR}"
	"${LIBFABRIC_INCLUDE_DIR}/Windows"
	${Qt5Core_INCLUDES}
	${Qt5Widgets_INCLUDES})

target_link_libraries(obs-cdi
	libobs
	"${CDI_LIB_DIR}/cdi_sdk.lib"
	"${CDI_LIB_DIR}/libfabric.lib"
	Qt5::Core
	Qt5::Widgets)

# Windows
if(WIN32)
	set(OBS_FRONTEND_LIB "OBS_FRONTEND_LIB-NOTFOUND" CACHE FILEPATH "OBS frontend library")
	if(OBS_FRONTEND_LIB EQUAL "OBS_FRONTEND_LIB-NOTFOUND")
		message(FATAL_ERROR "Could not find OBS Frontend API\'s library !")
	endif()

    if(MSVC)
        # Enable Multicore Builds and disable FH4 (to not depend on VCRUNTIME140_1.DLL)
	    add_definitions(/MP /d2FH4-)
    endif()

	add_definitions(/O2)
	string(REGEX REPLACE "/RTC(su|[1su])" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
	message("${CMAKE_CXX_FLAGS_RELEASE}")

	if(ARCH EQUAL 64)
		set(OBS_ARCH_NAME "64bit")
		set(OBS_BUILDDIR_ARCH "build64")
	else()
		set(OBS_ARCH_NAME "32bit")
		set(OBS_BUILDDIR_ARCH "build32")
		add_definitions(/arch:SSE2)
	endif()

	find_package(w32-pthreads REQUIRED)

	target_link_libraries(obs-cdi
		"${OBS_FRONTEND_LIB}"
		w32-pthreads)

	set(RELEASE_DIR "${PROJECT_SOURCE_DIR}/release")
	add_custom_command(TARGET obs-cdi POST_BUILD

			COMMAND  ("${CMAKE_COMMAND}" -E copy
				"$<TARGET_FILE:obs-cdi>"
				"${LIBOBS_INCLUDE_DIR}/../build/rundir/$<CONFIG>/obs-plugins/${OBS_ARCH_NAME}"
			)
			COMMAND  ("${CMAKE_COMMAND}" -E copy
				"${CDI_LIB_DIR}/cdi_sdk.dll"
				"${LIBOBS_INCLUDE_DIR}/../build/rundir/$<CONFIG>/bin/${OBS_ARCH_NAME}"
			)
			COMMAND  ("${CMAKE_COMMAND}" -E copy
				"${CDI_LIB_DIR}/cdi_sdk.pdb"
				"${LIBOBS_INCLUDE_DIR}/../build/rundir/$<CONFIG>/bin/${OBS_ARCH_NAME}"
			)
			COMMAND  ("${CMAKE_COMMAND}" -E copy
				"${CDI_LIB_DIR}/libfabric.dll"
				"${LIBOBS_INCLUDE_DIR}/../build/rundir/$<CONFIG>/bin/${OBS_ARCH_NAME}"
			)
			COMMAND  ("${CMAKE_COMMAND}" -E copy
				"${CDI_LIB_DIR}/libfabric.pdb"
				"${LIBOBS_INCLUDE_DIR}/../build/rundir/$<CONFIG>/bin/${OBS_ARCH_NAME}"
			)
			COMMAND  ("${CMAKE_COMMAND}" -E make_directory
				"${LIBOBS_INCLUDE_DIR}/../build/rundir/$<CONFIG>/data/obs-plugins/obs-cdi"
			)
			COMMAND  ("${CMAKE_COMMAND}" -E make_directory
				"${LIBOBS_INCLUDE_DIR}/../build/rundir/$<CONFIG>/data/obs-plugins/obs-cdi/locale"
			)
			COMMAND  ("${CMAKE_COMMAND}" -E copy_directory
				"${PROJECT_SOURCE_DIR}/locale"
				"${LIBOBS_INCLUDE_DIR}/../build/rundir/$<CONFIG>/data/obs-plugins/obs-cdi/locale"
			)
	)
endif()
