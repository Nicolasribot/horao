# - Find the PostgreSQL installation.
# In Windows, we make the assumption that, if the PostgreSQL files are installed, the default directory
# will be C:\Program Files\PostgreSQL.
#
# This module defines
#  PostgreSQL_LIBRARIES - the PostgreSQL libraries needed for linking
#  PostgreSQL_INCLUDE_DIRS - the directories of the PostgreSQL headers
#  PostgreSQL_VERSION_STRING - the version of PostgreSQL found (since CMake 2.8.8)

#=============================================================================
# Copyright 2004-2009 Kitware, Inc.
#
# Distributed under the OSI-approved BSD License (the "License");
# see accompanying file Copyright.txt for details.
#
# This software is distributed WITHOUT ANY WARRANTY; without even the
# implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
# See the License for more information.
#=============================================================================
# (To distribute this file outside of CMake, substitute the full
#  License text for the above reference.)

# ----------------------------------------------------------------------------
# History:
# This module is derived from the module originally found in the VTK source tree.
#
# ----------------------------------------------------------------------------
# Note:
# PostgreSQL_ADDITIONAL_VERSIONS is a variable that can be used to set the
# version mumber of the implementation of PostgreSQL.
# In Windows the default installation of PostgreSQL uses that as part of the path.
# E.g C:\Program Files\PostgreSQL\8.4.
# Currently, the following version numbers are known to this module:
# "9.1" "9.0" "8.4" "8.3" "8.2" "8.1" "8.0"
#
# To use this variable just do something like this:
# set(PostgreSQL_ADDITIONAL_VERSIONS "9.2" "8.4.4")
# before calling find_package(PostgreSQL) in your CMakeLists.txt file.
# This will mean that the versions you set here will be found first in the order
# specified before the default ones are searched.
#
# ----------------------------------------------------------------------------
# You may need to manually set:
#  PostgreSQL_INCLUDE_DIR  - the path to where the PostgreSQL include files are.
#  PostgreSQL_LIBRARY_DIR  - The path to where the PostgreSQL library files are.
# If FindPostgreSQL.cmake cannot find the include files or the library files.
#
# ----------------------------------------------------------------------------
# The following variables are set if PostgreSQL is found:
#  PostgreSQL_FOUND         - Set to true when PostgreSQL is found.
#  PostgreSQL_INCLUDE_DIRS  - Include directories for PostgreSQL
#  PostgreSQL_LIBRARY_DIRS  - Link directories for PostgreSQL libraries
#  PostgreSQL_LIBRARIES     - The PostgreSQL libraries.
#
# ----------------------------------------------------------------------------
# If you have installed PostgreSQL in a non-standard location.
# (Please note that in the following comments, it is assumed that <Your Path>
# points to the root directory of the include directory of PostgreSQL.)
# Then you have three options.
# 1) After CMake runs, set PostgreSQL_INCLUDE_DIR to <Your Path>/include and
#    PostgreSQL_LIBRARY_DIR to wherever the library pq (or libpq in windows) is
# 2) Use CMAKE_INCLUDE_PATH to set a path to <Your Path>/PostgreSQL<-version>. This will allow find_path()
#    to locate PostgreSQL_INCLUDE_DIR by utilizing the PATH_SUFFIXES option. e.g. In your CMakeLists.txt file
#    set(CMAKE_INCLUDE_PATH ${CMAKE_INCLUDE_PATH} "<Your Path>/include")
# 3) Set an environment variable called ${PostgreSQL_ROOT} that points to the root of where you have
#    installed PostgreSQL, e.g. <Your Path>.
#
# ----------------------------------------------------------------------------

set(LibPQ_INCLUDE_PATH_DESCRIPTION "top-level directory containing the PostgreSQL include directories. E.g /usr/local/include/PostgreSQL/8.4 or C:/Program Files/PostgreSQL/8.4/include")
set(LibPQ_INCLUDE_DIR_MESSAGE "Set the PostgreSQL_INCLUDE_DIR cmake cache entry to the ${PostgreSQL_INCLUDE_PATH_DESCRIPTION}")
set(LibPQ_LIBRARY_PATH_DESCRIPTION "top-level directory containing the PostgreSQL libraries.")
set(LibPQ_LIBRARY_DIR_MESSAGE "Set the PostgreSQL_LIBRARY_DIR cmake cache entry to the ${PostgreSQL_LIBRARY_PATH_DESCRIPTION}")
set(LibPQ_ROOT_DIR_MESSAGE "Set the PostgreSQL_ROOT system variable to where PostgreSQL is found on the machine E.g C:/Program Files/PostgreSQL/8.4")


set(LibPQ_KNOWN_VERSIONS ${LibPQ_ADDITIONAL_VERSIONS}
    "9.1" "9.0" "8.4" "8.3" "8.2" "8.1" "8.0")

# Define additional search paths for root directories.
if ( WIN32 )
  foreach (suffix ${LibPQ_KNOWN_VERSIONS} )
    set(LibPQ_ADDITIONAL_SEARCH_PATHS ${LibPQ_ADDITIONAL_SEARCH_PATHS} "C:/Program Files/PostgreSQL/${suffix}" )
  endforeach()
endif()
set( LibPQ_ROOT_DIRECTORIES
   ENV LibPQ_ROOT
   ${LibPQ_ROOT}
   ${LibPQ_ADDITIONAL_SEARCH_PATHS}
)

#
# Look for an installation.
#
find_path(LibPQ_INCLUDE_DIR
  NAMES libpq-fe.h
  PATHS
   # Look in other places.
   ${LibPQ_ROOT_DIRECTORIES}
  PATH_SUFFIXES
    pgsql
    postgresql
    include
  # Help the user find it if we cannot.
  DOC "The ${LibPQ_INCLUDE_DIR_MESSAGE}"
)

# The PostgreSQL library.
set (LibPQ_LIBRARY_TO_FIND pq)
# Setting some more prefixes for the library
set (LibPQ_LIB_PREFIX "")
if ( WIN32 )
  set (LibPQ_LIB_PREFIX ${LibPQ_LIB_PREFIX} "lib")
  set ( LibPQ_LIBRARY_TO_FIND ${LibPQ_LIB_PREFIX}${LibPQ_LIBRARY_TO_FIND})
endif()

find_library( LibPQ_LIBRARY
 NAMES ${LibPQ_LIBRARY_TO_FIND}
 PATHS
   ${LibPQ_ROOT_DIRECTORIES}
 PATH_SUFFIXES
   lib
)
get_filename_component(LibPQ_LIBRARY_DIR ${LibPQ_LIBRARY} PATH)

if (LibPQ_INCLUDE_DIR AND EXISTS "${LibPQ_INCLUDE_DIR}/pg_config.h")
  file(STRINGS "${LibPQ_INCLUDE_DIR}/pg_config.h" pgsql_version_str
       REGEX "^#define[\t ]+PG_VERSION[\t ]+\".*\"")

  string(REGEX REPLACE "^#define[\t ]+PG_VERSION[\t ]+\"([^\"]*)\".*" "\\1"
         LibPQ_VERSION_STRING "${pgsql_version_str}")
  unset(pgsql_version_str)
endif()

set(LibPQ_INCLUDE_DIRS ${LibPQ_INCLUDE_DIR} )
set(LibPQ_LIBRARY_DIRS ${LibPQ_LIBRARY_DIR} )
set(LibPQ_LIBRARIES ${LibPQ_LIBRARY_TO_FIND})

mark_as_advanced(LibPQ_INCLUDE_DIR LibPQ_LIBRARY )
