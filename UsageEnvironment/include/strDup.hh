/**********
This library is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the
Free Software Foundation; either version 3 of the License, or (at your
option) any later version. (See <http://www.gnu.org/copyleft/lesser.html>.)

This library is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
more details.

You should have received a copy of the GNU Lesser General Public License
along with this library; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
**********/

#ifndef _STRDUP_HH
#define _STRDUP_HH

// Copyright (c) 1996-2023 Live Networks, Inc.  All rights reserved.
// A C++ equivalent to the standard C routine "strdup()".
// This generates a char* that can be deleted using "delete[]"
// Header

#include <string.h>

#ifdef _WIN32
# ifdef LIVE555_LIB
#  define LIVE555_EXPORT __declspec(dllexport)
# else
#  define LIVE555_EXPORT __declspec(dllimport)
# endif
#else
# define LIVE555_EXPORT
#endif

LIVE555_EXPORT char* strDup(char const* str);
// Note: strDup(NULL) returns NULL

LIVE555_EXPORT char* strDupSize(char const* str);
// Like "strDup()", except that it *doesn't* copy the original.
// (Instead, it just allocates a string of the same size as the original.)

LIVE555_EXPORT char* strDupSize(char const* str, size_t& resultBufSize);
// An alternative form of "strDupSize()" that also returns the size of the allocated buffer.

#endif
