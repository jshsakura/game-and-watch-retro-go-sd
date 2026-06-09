#pragma once

// This build's miniz.h ships without the optional zlib-compatible name
// aliases, but lupng.c was written against the zlib API. Map the handful of
// names lupng uses onto their miniz equivalents (mz_stream shares zlib's
// field names, so struct access works unchanged).
#include "miniz.h"

#define z_stream     mz_stream
#define inflateInit  mz_inflateInit
#define inflate      mz_inflate
#define inflateEnd   mz_inflateEnd
#define deflateInit  mz_deflateInit
#define deflate      mz_deflate
#define deflateEnd   mz_deflateEnd

#define Z_OK         MZ_OK
#define Z_NO_FLUSH   MZ_NO_FLUSH
#define Z_FINISH     MZ_FINISH
#define Z_STREAM_END MZ_STREAM_END
#define Z_BUF_ERROR  MZ_BUF_ERROR
#define Z_NEED_DICT  MZ_NEED_DICT
