cmake_minimum_required( VERSION 2.8 )
project( 3dstack )

set( CMAKE_DEBUG_POSTFIX "d" )
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)
set(LIBRARY_OUTPUT_PATH ${PROJECT_BINARY_DIR}/lib)

enable_testing()

#----------------------------------------------------------------------------
# build options
#----------------------------------------------------------------------------
# Set a default build type if none was specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Setting build type to 'Debug' as none was specified.")
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
  # Set the possible values of build type for cmake-gui
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo")
endif()

#-----------------------------------------------------------
# dependencies
#-----------------------------------------------------------

find_package( OpenSceneGraph COMPONENTS osgQt osgViewer osgGA osgDB osgUtil osgText osgTerrain REQUIRED )
#set (OPENSCENEGRAPH_LIBRARIES ${OPENSCENEGRAPH_LIBRARIES} osgPPU)
find_package( OpenGL REQUIRED )
find_package( GDAL REQUIRED )
#find_package( LibXml2 REQUIRED)
set( CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake;${CMAKE_MODULE_PATH}" )
find_package( LibPQ REQUIRED)
find_package( LWGEOM REQUIRED)

#-- note that SYSTEM turns -I/path to -isystem and avoid warnings in external libs
include_directories( SYSTEM 
    ${OPENSCENEGRAPH_INCLUDE_DIRS} 
    #${LIBXML2_INCLUDE_DIR} 
    ${LibPQ_INCLUDE_DIR}
    ${GDAL_INCLUDE_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/src
)

#-- Warnings, frounding-math and gprof  ------------------------------------------
if(MSVC)
	# Force to always compile with W4
	if(CMAKE_CXX_FLAGS MATCHES "/W[0-4]")
		string(REGEX REPLACE "/W[0-4]" "/W4" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
	else()
		set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4")
	endif()
elseif(CMAKE_COMPILER_IS_GNUCC OR CMAKE_COMPILER_IS_GNUCXX)
	# Update if necessary
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++0x -Wall -Wextra -Wno-long-long -pedantic -Wpointer-arith -Wcast-align -Wcast-qual -Woverloaded-virtual -Wformat=2 -Winit-self -Wmissing-include-dirs -Wwrite-strings -Wno-error=undef -Wshadow")#-Wfloat-equal -Wconversion -Wshadow 
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror -Wno-error=format")
endif()

include(CheckFunctionExists)
check_function_exists(strcasestr HAVE_STRCASESTR)
if(HAVE_STRCASESTR)
 add_definitions("-DHAVE_STRCASESTR")
endif()

add_subdirectory( src )
add_subdirectory( qgis_plugin )

add_test( Style_test ${CMAKE_SOURCE_DIR}/script/test-style-horao.sh )


