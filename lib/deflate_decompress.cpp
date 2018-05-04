/*
 * deflate_decompress.c - a decompressor for DEFLATE
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
 *
 * ---------------------------------------------------------------------------
 *
 * This is a highly optimized DEFLATE decompressor.  When compiled with gcc on
 * x86_64, it decompresses data in about 52% of the time of zlib (48% if BMI2
 * instructions are available).  On other architectures it should still be
 * significantly faster than zlib, but the difference may be smaller.
 *
 * Why this is faster than zlib's implementation:
 *
 * - Word accesses rather than byte accesses when reading input
 * - Word accesses rather than byte accesses when copying matches
 * - Faster Huffman decoding combined with various DEFLATE-specific tricks
 * - Larger bitbuffer variable that doesn't need to be filled as often
 * - Other optimizations to remove unnecessary branches
 * - Only full-buffer decompression is supported, so the code doesn't need to
 *   support stopping and resuming decompression.
 * - On x86_64, compile a version of the decompression routine using BMI2
 *   instructions and use it automatically at runtime when supported.
 */


#include <pthread.h>

#include "assert.hpp"

#include "input_stream.hpp"
#include "decompressor.hpp"
#include "deflate_window.hpp"
#include "synchronizer.hpp"

#include "libdeflate.h"


/*****************************************************************************
 *                         Main decompression routine
 *****************************************************************************/

template<typename OutWindow, typename might>
inline bool do_uncompressed(InputStream& in_stream, OutWindow& out, const might&) {
    /* Uncompressed block: copy 'len' bytes literally from the input
     * buffer to the output buffer.  */
    in_stream.align_input();

    if (unlikely(in_stream.available() < 4))
    {
        PRINT_DEBUG("bad block, uncompressed check less than 4 bytes in input\n");
        return false;
    }


    u16 len = in_stream.pop_u16();
    u16 nlen = in_stream.pop_u16();

    if (might::fail_if(len != (u16)~nlen))
    {
        PRINT_DEBUG("bad uncompressed block: len encoding check\n");
        return false;
    }

    if (unlikely(len > in_stream.available()))
    {
        PRINT_DEBUG("bad uncompressed block: len bigger than input stream \n");
        return false;
    }

    if(might::fail_if(!out.copy(in_stream, len))) {
        PRINT_DEBUG("bad uncompressed block: rejected by output window (non-ascii)\n");
        return false;
    };
    return true;
}

template<typename might>
static inline bool
prepare_dynamic(struct libdeflate_decompressor * restrict d,
                                              InputStream& in_stream,
                                              const might& might_tag) {


    /* The order in which precode lengths are stored.  */
    static constexpr u8 deflate_precode_lens_permutation[DEFLATE_NUM_PRECODE_SYMS] = {
            16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };

    /* Read the codeword length counts.  */
    unsigned num_litlen_syms = in_stream.pop_bits(5) + 257;
    unsigned num_offset_syms = in_stream.pop_bits(5) + 1;
    const unsigned num_explicit_precode_lens = in_stream.pop_bits(4) + 4;

    /* Read the precode codeword lengths.  */
    in_stream.ensure_bits<DEFLATE_NUM_PRECODE_SYMS * 3>();

    for (unsigned i = 0; i < num_explicit_precode_lens; i++)
            d->u.precode_lens[deflate_precode_lens_permutation[i]] = in_stream.pop_bits(3);

    for (unsigned i = num_explicit_precode_lens; i < DEFLATE_NUM_PRECODE_SYMS; i++)
            d->u.precode_lens[deflate_precode_lens_permutation[i]] = 0;

    /* Build the decode table for the precode.  */
    if (might::fail_if(!build_precode_decode_table(d, might_tag)))
        return false;

    /* Expand the literal/length and offset codeword lengths.  */
    for (unsigned i = 0; i < num_litlen_syms + num_offset_syms; ) {
            in_stream.ensure_bits<DEFLATE_MAX_PRE_CODEWORD_LEN + 7>();

            /* (The code below assumes that the precode decode table
             * does not have any subtables.)  */
            //static_assert(PRECODE_TABLEBITS == DEFLATE_MAX_PRE_CODEWORD_LEN);

            /* Read the next precode symbol.  */
            const u32 entry = d->u.l.precode_decode_table[in_stream.bits(DEFLATE_MAX_PRE_CODEWORD_LEN)];
            in_stream.remove_bits(entry & HUFFDEC_LENGTH_MASK);
            const unsigned presym = entry >> HUFFDEC_RESULT_SHIFT;

            if (presym < 16) {
                    /* Explicit codeword length  */
                    d->u.l.lens[i++] = presym;
                    continue;
            }

            /* Run-length encoded codeword lengths  */

            /* Note: we don't need verify that the repeat count
             * doesn't overflow the number of elements, since we
             * have enough extra spaces to allow for the worst-case
             * overflow (138 zeroes when only 1 length was
             * remaining).
             *
             * In the case of the small repeat counts (presyms 16
             * and 17), it is fastest to always write the maximum
             * number of entries.  That gets rid of branches that
             * would otherwise be required.
             *
             * It is not just because of the numerical order that
             * our checks go in the order 'presym < 16', 'presym ==
             * 16', and 'presym == 17'.  For typical data this is
             * ordered from most frequent to least frequent case.
             */
            if (presym == 16) {
                    /* Repeat the previous length 3 - 6 times  */
                    if (might::fail_if(!(i != 0)))
                    {
                        PRINT_DEBUG("fail at (i!=0)\n");
                        return false;
                    }
                    const u8 rep_val = d->u.l.lens[i - 1];
                    const unsigned rep_count = 3 + in_stream.pop_bits(2);
                    d->u.l.lens[i + 0] = rep_val;
                    d->u.l.lens[i + 1] = rep_val;
                    d->u.l.lens[i + 2] = rep_val;
                    d->u.l.lens[i + 3] = rep_val;
                    d->u.l.lens[i + 4] = rep_val;
                    d->u.l.lens[i + 5] = rep_val;
                    i += rep_count;
            } else if (presym == 17) {
                    /* Repeat zero 3 - 10 times  */
                    const unsigned rep_count = 3 + in_stream.pop_bits(3);
                    d->u.l.lens[i + 0] = 0;
                    d->u.l.lens[i + 1] = 0;
                    d->u.l.lens[i + 2] = 0;
                    d->u.l.lens[i + 3] = 0;
                    d->u.l.lens[i + 4] = 0;
                    d->u.l.lens[i + 5] = 0;
                    d->u.l.lens[i + 6] = 0;
                    d->u.l.lens[i + 7] = 0;
                    d->u.l.lens[i + 8] = 0;
                    d->u.l.lens[i + 9] = 0;
                    i += rep_count;
            } else {
                    /* Repeat zero 11 - 138 times  */
                    const unsigned rep_count = 11 + in_stream.pop_bits(7);
                    memset(&d->u.l.lens[i], 0,
                           rep_count * sizeof(d->u.l.lens[i]));
                    i += rep_count;
            }
    }

    if (!build_offset_decode_table(d, num_litlen_syms, num_offset_syms, might_tag))
    {
        PRINT_DEBUG("fail at build_offset_decode_table(d, num_litlen_syms, num_offset_syms)\n");
        return false;
    }
    if (!build_litlen_decode_table(d, num_litlen_syms, num_offset_syms, might_tag))
    {
        PRINT_DEBUG("fail at build_litlen_decode_table(d, num_litlen_syms, num_offset_syms)\n");
        return false;
    }

    return true;
}




/* return true if block decompression went smoothly, false if not (probably due to corrupt data) */
template<typename OutWindow, typename might=ShouldSucceed>
inline bool do_block(struct libdeflate_decompressor * restrict main_d, InputStream& in_stream, OutWindow& out, const might& might_tag={})
{
    libdeflate_decompressor *restrict cur_d;
    /* Starting to read the next block.  */
    in_stream.ensure_bits<1 + 2 + 5 + 5 + 4>();

    if (unlikely(in_stream.available() == 0)) // Rayan: i've added that check but i doubt it's useful (actually.. maybe it is, if we have been unable to decompress any block..)
    {
        fprintf(stderr,"reached end of file\n");
        in_stream.reached_final_block = true;
        return false;
    }

    /* BFINAL: 1 bit  */
    in_stream.reached_final_block = in_stream.pop_bits(1);

    bool ret;
    /* BTYPE: 2 bits  */
    switch(in_stream.pop_bits(2)) {
    case DEFLATE_BLOCKTYPE_DYNAMIC_HUFFMAN:
        ret = prepare_dynamic(main_d, in_stream, might_tag);
        cur_d = main_d;
        if (might::fail_if(!ret))
            return false;
        break;

    case DEFLATE_BLOCKTYPE_UNCOMPRESSED:
        return do_uncompressed(in_stream, out, might_tag)
            && out.notify_end_block(in_stream);
        break;

    case DEFLATE_BLOCKTYPE_STATIC_HUFFMAN:
        //prepare_static(d);
        cur_d = main_d->static_decompressor;
        break;

    default:
        return might::fail_if(false);
    }

    /* Decompressing a Huffman block (either dynamic or static)  */
    DEBUG_FIRST_BLOCK(fprintf(stderr,"trying to decode huffman block\n");)

    /* The main DEFLATE decode loop  */
    for (;;) {
        /* Decode a litlen symbol.  */
        in_stream.ensure_bits<DEFLATE_MAX_LITLEN_CODEWORD_LEN>();
        //FIXME: entry should be const
        u32 entry = cur_d->u.litlen_decode_table[in_stream.bits(LITLEN_TABLEBITS)];
        if (entry & HUFFDEC_SUBTABLE_POINTER) {
            /* Litlen subtable required (uncommon case)  */
            in_stream.remove_bits(LITLEN_TABLEBITS);
            entry = cur_d->u.litlen_decode_table[ ((entry >> HUFFDEC_RESULT_SHIFT) & 0xFFFF)
                                                  + in_stream.bits(entry & HUFFDEC_LENGTH_MASK)];
        }
        in_stream.remove_bits(entry & HUFFDEC_LENGTH_MASK);
        //PRINT_DEBUG("in_stream position %x\n",in_stream.in_next);
        if (entry & HUFFDEC_LITERAL) {
            /* Literal  */
            if(unlikely(out.available() == 0))
            {
                if(might::fail_if(out.flush() == 0))
                    return false;
            }

            if(might::fail_if(!out.push(byte(entry >> HUFFDEC_RESULT_SHIFT))))
            {
                return false;
            }

            //fprintf(stderr,"literal: %c\n",byte(entry >> HUFFDEC_RESULT_SHIFT)); // this is indeed the plaintext decoded character, good to know
            continue;
        }

        /* Match or end-of-block  */
        entry >>= HUFFDEC_RESULT_SHIFT;
        in_stream.ensure_bits<in_stream.bitbuf_max_ensure>();

        /* Pop the extra length bits and add them to the length base to
         * produce the full length.  */
        const u32 length = (entry >> HUFFDEC_LENGTH_BASE_SHIFT) +
                 in_stream.pop_bits(entry & HUFFDEC_EXTRA_LENGTH_BITS_MASK);

        /* The match destination must not end after the end of the
         * output buffer.  For efficiency, combine this check with the
         * end-of-block check.  We're using 0 for the special
         * end-of-block length, so subtract 1 and it turn it into
         * SIZE_MAX.  */
        //static_assert(HUFFDEC_END_OF_BLOCK_LENGTH == 0);
        if (unlikely(length - 1 >= out.available())) {
                if (likely(length == HUFFDEC_END_OF_BLOCK_LENGTH))
                {
                    return out.notify_end_block(in_stream); // Block done
                } else { // Needs flushing
                    if(unlikely(out.flush() == 0)) {
                        return false;
                    }
                    assert(length <= out.available());
                }
        }
        assert(length > 0); // length == 0 => EOB case should be handled here

        // if we end up here, it means we're at a match

        /* Decode the match offset.  */
        entry = cur_d->offset_decode_table[in_stream.bits(OFFSET_TABLEBITS)];
        if (entry & HUFFDEC_SUBTABLE_POINTER) {
                /* Offset subtable required (uncommon case)  */
                in_stream.remove_bits(OFFSET_TABLEBITS);
                entry = cur_d->offset_decode_table[ ((entry >> HUFFDEC_RESULT_SHIFT) & 0xFFFF) +
                                                    + in_stream.bits(entry & HUFFDEC_LENGTH_MASK)];
        }
        in_stream.remove_bits(entry & HUFFDEC_LENGTH_MASK);
        entry >>= HUFFDEC_RESULT_SHIFT;

        /* Pop the extra offset bits and add them to the offset base to
         * produce the full offset.  */
        const u32 offset = (entry & HUFFDEC_OFFSET_BASE_MASK) +
                 in_stream.pop_bits(entry >> HUFFDEC_EXTRA_OFFSET_BITS_SHIFT);


        /* Copy the match: 'length' bytes at 'out_next - offset' to
         * 'out_next'.  */
        if(might::fail_if(!out.copy_match(length, offset)))
        {
            return false;
        }

    }
}

/* if we need to stop 20 blocks after some point, and that point has been reached, setup a counter */
bool handle_until(size_t until, int &until_counter, size_t position)
{
    if (until_counter == -1 && position > until)
        until_counter = 20;
    if (until_counter > 0)
    {
        until_counter--;
        if (until_counter == 0)
        {
            fprintf(stderr,"stopping 20 blocks after specified position\n");
            return true;
        }

    }
    return false;
}



template<typename Window>
std::pair<Window, size_t> do_skip(struct libdeflate_decompressor * restrict d,
                                  InputStream& in_stream,
                                  const size_t skip=0,
                                  const unsigned required_valid_blocks=Window::buffer_size>>18, // Number of blocks that can sefelly fit without flushing
                                  const size_t max_bits_skip=size_t(1)<<(3+20), // 1MiB
                                  const size_t min_block_size=1<<10            // 10KiB
                                  )
{
    if(skip == 0) return { Window(), size_t(0) };
    in_stream.skip(skip);

    SyncingDeflateWindow<Window> out_window;
    size_t bits_skipped = 0;
    for(;
        bits_skipped < max_bits_skip && in_stream.ensure_bits<1>();
        bits_skipped++, in_stream.remove_bits(1))
    {
        if(in_stream.bits(1)) // We don't except to find a final block
            continue;

        InputStream cur_in = in_stream;

//        if(cur_in.position_bits() >= 415018) {
//            fprintf(stderr, "Block reached!!\n");
//        })

        if(unlikely(do_block(d, cur_in, out_window, ShouldFail{})
                    && out_window.size() - out_window.context_size > min_block_size))
        {
            bool went_fine = true;
            for(unsigned trial=0;
                trial < required_valid_blocks
                    && likely(went_fine)
                    && likely(!cur_in.reached_final_block) ;
                trial++)
            {
                    went_fine &= do_block(d, cur_in, out_window, ShouldSucceed{});
            }
            if(likely(went_fine
                      && cur_in.reached_final_block == (cur_in.available() == 0) ))
            {
                fprintf(stderr, "%lu %lu %lu\n", bits_skipped, in_stream.position_bits(), cur_in.position_bits());
                in_stream = cur_in;
                return { std::move(out_window), bits_skipped };
            }
            fprintf(stderr,"FP %lu\n", out_window.size()-out_window.context_size);
        }

        out_window.clear();
    }

    fprintf(stderr,"Failled to do %lu bytes skip:\n"
                   "\tbits skipped:\t\t%lu/%lu\n"
                   "\tinput remaning bytes:\t%lu\n",
            skip,
            bits_skipped,
            max_bits_skip,
            in_stream.available());
    exit(1);
}

void print_block_boundaries(struct libdeflate_decompressor * restrict d,
                            const InputStream& in_stream,
                            size_t nb_blocks=~size_t(0)) {
    SyncingDeflateWindow<DeflateWindow<>> out_window;
    InputStream cur_in = in_stream;
    for(size_t i=0 ; i < nb_blocks && !cur_in.reached_final_block; i++) {
        fprintf(stderr, "Block bundary: %12lubits, remains: %12lubits %12lu %12lu\n",
                cur_in.position_bits(), cur_in.available_bits(), cur_in.position_bits() + cur_in.available_bits(), 8*(cur_in.in_end - cur_in.begin));
        assert(do_block(d, cur_in, out_window, MustSucceed{}));
        out_window.clear();
    }
    fprintf(stderr, "Block bundary: %12lubits, remains: %12lubits %12lu %12lu\n",
            cur_in.position_bits(), cur_in.available_bits(), cur_in.position_bits() + cur_in.available_bits(), 8*(cur_in.in_end - cur_in.begin));
}

// Original API:

LIBDEFLATEAPI enum libdeflate_result
libdeflate_deflate_decompress(struct libdeflate_decompressor * restrict d,
			      const byte * restrict const in, size_t in_nbytes,
			      byte * restrict const out, size_t out_nbytes_avail,
			      size_t *actual_out_nbytes_ret,
                  synchronizer* stop,  // indicating where to stop
                  synchronizer* prev_sync, // for passing our first extracted sequence coordinate to the previous thread
                  size_t skip, size_t until)
{
    InputStream in_stream(in, in_nbytes);
    stderr = stdout;

    //byte *out_next = out;
    //byte * const out_end = out_next + out_nbytes_avail;

    print_block_boundaries(d, in_stream);

    while(!in_stream.reached_final_block) {
        auto pair = do_skip<DeflateWindow<>>(d, in_stream, skip);
        fprintf(stderr, "Managed to skip after %lu bits\n"
                "\t out_window.size(): %lu\n"
                , pair.second, pair.first.size());
        //exit(0);
    }

#if 0
    InstrDeflateWindow out_window;


    // blocks counter
    int failed_decomp_counter = 0;
    uint64_t decoded_blocks = 0;

    // handle skipping
    signed int until_counter = -1;
    int nb_to_record = 10;

    fprintf(stderr, "Thread %lu started with a skip of %lu bytes\n", pthread_self(), skip);

    /* Skipping user-set amount of bytes, after the header of course */
    if (skip)
    {
        in_stream.in_next += skip; // TODO: will probably not be valid when input is a stream
    }

    /* we will dump some blocks to disk. but first, remove existing ones */
    for (int i = 0; i < nb_to_record; i++)
        remove( ("/tmp/block" + std::to_string(i)+ ".dump").c_str() );

    bool is_final_block = false, aligned = false;
    InputStream backup_in(in_stream);

    do {
        //PRINT_DEBUG("before block,             out window %x - %x\n", out_window.next, out_window.buffer_end);

        size_t block_inpos = in_stream.position();
        bool went_fine = do_block(d, in_stream, out_window, is_final_block, ShouldFail{});
        if(unlikely(!aligned && went_fine)) {
            went_fine &= out_window.check_ascii();
            if(went_fine) {
                PRINT_DEBUG("First sync block at %d %d\n", in_stream.position(), in_stream.position_bits());
            }
        }

        if (likely(went_fine))
        {
            decoded_blocks++;
            aligned = true; // found a way to fully decompress a block, seems that we're good

            //fprintf(stderr,"block decompressed!\n");//, out_window.buffer);
            failed_decomp_counter = 0; // reset failed block counter

            long long position = in_stream.position();
            if (handle_until(until, until_counter, position))
                break;

            if(stop != nullptr && !is_final_block) {
                is_final_block |= stop->caught_up_block(block_inpos);
                if(is_final_block)
                    fprintf(stderr, "thread %lu stoped at %lu\n", pthread_self(), block_inpos);
            }

            if (out_window.fully_reconstructed == false)
            {
                out_window.check_fully_reconstructed_sequences(stop, is_final_block); // see if we have uncertainties in nucleotides
                if (out_window.fully_reconstructed) {
                    if(prev_sync != nullptr) {
                        fprintf(stderr, "Thread %lu found it's first sequence in block %lu at position %u\n",
                                pthread_self(), block_inpos, out_window.first_seq_block_pos);
                        prev_sync->signal_first_decoded_sequence(block_inpos, out_window.first_seq_block_pos);
                    }
                    fprintf(stderr,"successfully decoded reads at decoded block %ld\n",decoded_blocks);
                }
            } else  { //FIXME: we want all the sequences, right ?
                out_window.check_fully_reconstructed_sequences(stop, is_final_block);
            }

            out_window.notify_end_block(is_final_block, in_stream);
        }
        else
        {
            if (unlikely(aligned))
            {
                fprintf(stderr,"unexpected error, bad block after we thought we had found a correct one\n");
                // we're not ready to support that case, as the buffer at this point contains something else than an empty context
                exit(1);
            }
            if (unlikely(failed_decomp_counter > 300000*8))
            {
                fprintf(stderr,"giving up, can't random-access this gzipped file even when bruteforcing next %d putative block positions\n", 300000*8);
                exit(1);
            }
            failed_decomp_counter++;
            PRINT_DEBUG("couldn't decompress that block, increasing fail count to %d\n",failed_decomp_counter);

            // Restore input stream one bit ahead of last try
            backup_in.ensure_bits<1>(); // to make sure there is at least one bit to pop
            backup_in.remove_bits(1);
            in_stream = backup_in;

            // Restore output window initial state (zero position and stats)
            out_window.clear();
            PRINT_DEBUG("restored after bad block\n");
        }
    } while((!aligned) || (aligned && !is_final_block));

    out_window.flush();

#endif

    return LIBDEFLATE_SUCCESS;
}
