# Copyright (c) 2015, 2016, Oracle and/or its affiliates. All rights reserved.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

INCLUDE (CheckStructHasMember)

#
# Endianess
#

INCLUDE(CheckTypeSize)
INCLUDE(CheckIncludeFiles)
INCLUDE(CheckSymbolExists)
INCLUDE(TestBigEndian)

CHECK_INCLUDE_FILES(stdint.h HAVE_STDINT_H)

# depending on the platform, we may or may not have this file
CHECK_INCLUDE_FILES(endian.h HAVE_ENDIAN_H)

# The header for glibc versions less than 2.9 will not
# have the endian conversion macros defined
IF(HAVE_ENDIAN_H)
  CHECK_SYMBOL_EXISTS(le64toh endian.h HAVE_LE64TOH)
  CHECK_SYMBOL_EXISTS(le32toh endian.h HAVE_LE32TOH)
  CHECK_SYMBOL_EXISTS(le16toh endian.h HAVE_LE16TOH)
  CHECK_SYMBOL_EXISTS(htole64 endian.h HAVE_HTOLE64)
  CHECK_SYMBOL_EXISTS(htole32 endian.h HAVE_HTOLE32)
  CHECK_SYMBOL_EXISTS(htole16 endian.h HAVE_HTOLE16)
  IF(HAVE_LE32TOH AND HAVE_LE16TOH AND HAVE_LE64TOH AND
     HAVE_HTOLE64 AND HAVE_HTOLE32 AND HAVE_HTOLE16)
    SET(HAVE_ENDIAN_CONVERSION_MACROS 1)
  ENDIF()
ENDIF()

IF(HAVE_STDINT_H)
  SET(CMAKE_EXTRA_INCLUDE_FILES stdint.h)
  CHECK_TYPE_SIZE("long long" LONG_LONG)
  CHECK_TYPE_SIZE(long LONG)
  CHECK_TYPE_SIZE(int INT)
  SET(CMAKE_EXTRA_INCLUDE_FILES)
ENDIF()

CHECK_TYPE_SIZE("void *"    SIZEOF_VOIDP)
CHECK_TYPE_SIZE("char *"    SIZEOF_CHARP)
CHECK_TYPE_SIZE("long"      SIZEOF_LONG)
CHECK_TYPE_SIZE("short"     SIZEOF_SHORT)
CHECK_TYPE_SIZE("int"       SIZEOF_INT)
CHECK_TYPE_SIZE("long long" SIZEOF_LONG_LONG)
CHECK_TYPE_SIZE("off_t"     SIZEOF_OFF_T)
CHECK_TYPE_SIZE("time_t"    SIZEOF_TIME_T)

TEST_BIG_ENDIAN(IS_BIG_ENDIAN)
TEST_BIG_ENDIAN(WORDS_BIGENDIAN)

SET(HAVE_INT 1)
SET(HAVE_LONG 1)
SET(HAVE_LONG_LONG 1)

#
# Cross-platform directives
#

# Network interfaces
CHECK_STRUCT_HAS_MEMBER("struct sockaddr" sa_len sys/socket.h HAVE_STRUCT_SOCKADDR_SA_LEN)
CHECK_STRUCT_HAS_MEMBER("struct ifreq" ifr_name net/if.h HAVE_STRUCT_IFREQ_IFR_NAME)

IF (HAVE_STRUCT_IFREQ_IFR_NAME)
  ADD_DEFINITIONS(-DHAVE_STRUCT_IFREQ_IFR_NAME)
ENDIF()

IF (HAVE_STRUCT_SOCKADDR_SA_LEN)
  ADD_DEFINITIONS(-DHAVE_STRUCT_SOCKADDR_SA_LEN)
ENDIF()

#
# XDR related checks
#
CHECK_STRUCT_HAS_MEMBER("struct xdr_ops" x_putint32 rpc/xdr.h HAVE_XDR_OPS_X_PUTINT32)
CHECK_STRUCT_HAS_MEMBER("struct xdr_ops" x_getint32 rpc/xdr.h HAVE_XDR_OPS_X_GETINT32)
CHECK_C_SOURCE_COMPILES(
  "
  #include <rpc/types.h>
  int main(void) { rpc_inline_t x; return 0; }
  "
  HAVE_RPC_INLINE_T)

IF (HAVE_RPC_INLINE_T)
  ADD_DEFINITIONS(-DHAVE_RPC_INLINE_T)
ENDIF()

CHECK_C_SOURCE_COMPILES(
  "
  #include <rpc/rpc.h>
  int main(__const int *i){return *i;}
  "
  HAVE___CONST)

IF (HAVE___CONST)
  ADD_DEFINITIONS(-DHAVE___CONST)
ENDIF()

IF (HAVE_XDR_OPS_X_PUTINT32)
  ADD_DEFINITIONS(-DHAVE_XDR_OPS_X_PUTINT32)
ENDIF()

IF (HAVE_XDR_OPS_X_GETINT32)
  ADD_DEFINITIONS(-DHAVE_XDR_OPS_X_GETINT32)
ENDIF()

#
# Per OS specific directives
#

#
# Finally, include all OS specific directives
#
# Include the platform-specific file. To allow exceptions, this code
# looks for files in order of how specific they are. If there is, for
# example, a generic Linux.cmake and a version-specific
# Linux-2.6.28-11-generic, it will pick Linux-2.6.28-11-generic and
# include it. It is then up to the file writer to include the generic
# version if necessary.
FOREACH(_base
    ${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_VERSION}-${CMAKE_SYSTEM_PROCESSOR}
    ${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_VERSION}
    ${CMAKE_SYSTEM_NAME})
  SET(_file ${PROJECT_SOURCE_DIR}/cmake/os/${_base}.cmake)
  IF(EXISTS ${_file})
    INCLUDE(${_file})
    BREAK()
  ENDIF()
ENDFOREACH()
