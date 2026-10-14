// thirdparty/crypto/sha256.cpp includes <endian.h> on every non-MSVC compiler
// for __BYTE_ORDER. MinGW has no such header; Windows is little-endian.
#pragma once
#ifndef __LITTLE_ENDIAN
#define __LITTLE_ENDIAN 1234
#endif
#ifndef __BIG_ENDIAN
#define __BIG_ENDIAN 4321
#endif
#ifndef __BYTE_ORDER
#define __BYTE_ORDER __LITTLE_ENDIAN
#endif
