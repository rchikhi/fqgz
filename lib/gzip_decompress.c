/*
 * gzip_decompress.c - decompress with a gzip wrapper
 *
 * Originally public domain; changes after 2016-09-07 are copyrighted.
 *
 * Copyright 2016 Eric Biggers
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "gzip_constants.h"
#include "unaligned.h"

#include "libdeflate.h"
#include "synchronizer.hpp"
#include <vector>
#include <thread>

template<typename T>
bool
is_set(T word, T flag)
{
    return word & flag != T{ 0 };
}

LIBDEFLATEAPI enum libdeflate_result
libdeflate_gzip_decompress(struct libdeflate_decompressor* d,
                           const byte* in,
                           size_t in_nbytes,
                           byte* out,
                           size_t out_nbytes_avail,
                           size_t* actual_out_nbytes_ret,
                           unsigned nthreads,
                           size_t skip,
                           size_t until)
{
    const byte* in_next = in;
    const byte* const in_end = in_next + in_nbytes;
    byte flg;
    //	size_t actual_out_nbytes;
    enum libdeflate_result result;

    if (in_nbytes < GZIP_MIN_OVERHEAD)
        return LIBDEFLATE_BAD_DATA;

    /* ID1 */
    if (*in_next++ != GZIP_ID1)
        return LIBDEFLATE_BAD_DATA;
    /* ID2 */
    if (*in_next++ != GZIP_ID2)
        return LIBDEFLATE_BAD_DATA;
    /* CM */
    if (*in_next++ != GZIP_CM_DEFLATE)
        return LIBDEFLATE_BAD_DATA;
    flg = *in_next++;
    /* MTIME */
    in_next += 4;
    /* XFL */
    in_next += 1;
    /* OS */
    in_next += 1;

    if (bool(flg & GZIP_FRESERVED))
        return LIBDEFLATE_BAD_DATA;

    /* Extra field */
    if (bool(flg & GZIP_FEXTRA)) {
        u16 xlen = get_unaligned_le16(in_next);
        in_next += 2;

        if (in_end - in_next < (u32)xlen + GZIP_FOOTER_SIZE)
            return LIBDEFLATE_BAD_DATA;

        in_next += xlen;
    }

    /* Original file name (zero terminated) */
    if (bool(flg & GZIP_FNAME)) {
        while (*in_next++ != byte(0) && in_next != in_end)
            ;
        if (in_end - in_next < GZIP_FOOTER_SIZE)
            return LIBDEFLATE_BAD_DATA;
    }

    /* File comment (zero terminated) */
    if (bool(flg & GZIP_FCOMMENT)) {
        while (*in_next++ != byte(0) && in_next != in_end)
            ;
        if (in_end - in_next < GZIP_FOOTER_SIZE)
            return LIBDEFLATE_BAD_DATA;
    }

    /* CRC16 for gzip header */
    if (bool(flg & GZIP_FHCRC)) {
        in_next += 2;
        if (in_end - in_next < GZIP_FOOTER_SIZE)
            return LIBDEFLATE_BAD_DATA;
    }

    // Estimate size:
    size_t decoded_size = get_unaligned_le32(in_end - 4); // Size from footer (modulo 4GB)
    decoded_size += size_t((in_end - in) * 3.2) & ~((1UL << 32) - 1);
    // Compression ratio for the deflate stream
    double compression_ratio = double(decoded_size) / double(in_end - in_next - 8);
    PRINT_DEBUG("Deompressed size %lu, compression factor %f\n", decoded_size, compression_ratio);

    nthreads = std::min(1 + unsigned(in_nbytes >> 26), nthreads);
    if (nthreads <= 1) {
        /* Compressed data  */
        result = libdeflate_deflate_decompress(
          d, in_next, in_end - GZIP_FOOTER_SIZE - in_next, out, out_nbytes_avail, actual_out_nbytes_ret, nullptr, nullptr, skip, until);
    } else {
        PRINT_DEBUG("Using %u threads\n", nthreads);

        std::vector<std::thread> threads;
        threads.reserve(nthreads);
        std::vector<synchronizer> syncs(nthreads - 1);

        size_t first_chunk_size = ((in_end - in_next) - skip) / nthreads + (1UL << 24);
        size_t chunk_size = ((in_end - in_next) - first_chunk_size) / (nthreads - 1);

        size_t start = skip;
        synchronizer* prev_sync = nullptr;
        for (unsigned i = 0; i < nthreads; i++) {
            synchronizer* stop = i < nthreads - 1 ? &syncs[i] : nullptr;

            threads.emplace_back([=]() {
                libdeflate_decompressor* local_d = libdeflate_copy_decompressor(d);

                enum libdeflate_result local_result = libdeflate_deflate_decompress(
                  local_d, in_next, in_end - GZIP_FOOTER_SIZE - in_next, out, out_nbytes_avail, actual_out_nbytes_ret, stop, prev_sync, start, until);

                if (local_result != LIBDEFLATE_SUCCESS)
                    exit(LIBDEFLATE_SUCCESS); // FIXME: use futures to pass result

                libdeflate_free_decompressor(local_d);
            });

            prev_sync = stop;
            start += i == 0 ? first_chunk_size : chunk_size;
        }

        for (auto& thread : threads)
            thread.join();

        result = LIBDEFLATE_SUCCESS;
    }

    if (result != LIBDEFLATE_SUCCESS)
        return result;

    //	if (actual_out_nbytes_ret)
    //		actual_out_nbytes = *actual_out_nbytes_ret;
    //	else
    //		actual_out_nbytes = out_nbytes_avail;

    in_next = in_end - GZIP_FOOTER_SIZE;

    // Rayan: skipping those checks as we may not be decompressing the whole file
#if 0
	/* CRC32 */
 	if (libdeflate_crc32(0, out, actual_out_nbytes) !=
 	    get_unaligned_le32(in_next))
 		return LIBDEFLATE_BAD_DATA;
	in_next += 4;

	/* ISIZE */
	if ((u32)actual_out_nbytes != get_unaligned_le32(in_next))
		return LIBDEFLATE_BAD_DATA;
#endif

    return LIBDEFLATE_SUCCESS;
}
