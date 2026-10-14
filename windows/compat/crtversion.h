// FFmpeg's libavutil/internal.h includes the MSVC CRT's version header when its
// configuration says the C library is msvcrt. MinGW's UCRT has no such header,
// and the version test that follows is only meaningful for MSVC's CRT.
#pragma once
