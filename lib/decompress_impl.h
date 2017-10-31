/*
 * decompress_impl.h
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

/*
 * This is the main DEFLATE decompression routine.  See libdeflate.h for the
 * documentation.
 *
 * Note that the real code is in decompress_impl.h.  The part here just handles
 * calling the appropriate implementation depending on the CPU features at
 * runtime.
 */

#include <utility>
#include <stdexcept>

class DeflateException: public std::runtime_error {
public:
    DeflateException(enum libdeflate_result): runtime_error("DeflateException") {}
};

void prepare_dynamic(struct libdeflate_decompressor * restrict d,
                                              InputStream& in_stream) {


    /* The order in which precode lengths are stored.  */
    static constexpr u8 deflate_precode_lens_permutation[DEFLATE_NUM_PRECODE_SYMS] = {
            16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };

    /* Read the codeword length counts.  */
    static_assert(DEFLATE_NUM_LITLEN_SYMS == ((1 << 5) - 1) + 257);
    unsigned num_litlen_syms = in_stream.pop_bits(5) + 257;

    static_assert(DEFLATE_NUM_OFFSET_SYMS == ((1 << 5) - 1) + 1);
    unsigned num_offset_syms = in_stream.pop_bits(5) + 1;

    static_assert(DEFLATE_NUM_PRECODE_SYMS == ((1 << 4) - 1) + 4);
    const unsigned num_explicit_precode_lens = in_stream.pop_bits(4) + 4;

    /* Read the precode codeword lengths.  */
    static_assert(DEFLATE_MAX_PRE_CODEWORD_LEN == (1 << 3) - 1);

    in_stream.ensure_bits<DEFLATE_NUM_PRECODE_SYMS * 3>();

    for (unsigned i = 0; i < num_explicit_precode_lens; i++)
            d->u.precode_lens[deflate_precode_lens_permutation[i]] = in_stream.pop_bits(3);

    for (unsigned i = num_explicit_precode_lens; i < DEFLATE_NUM_PRECODE_SYMS; i++)
            d->u.precode_lens[deflate_precode_lens_permutation[i]] = 0;

    /* Build the decode table for the precode.  */
    assert(build_precode_decode_table(d));

    /* Expand the literal/length and offset codeword lengths.  */
    for (unsigned i = 0; i < num_litlen_syms + num_offset_syms; ) {
            in_stream.ensure_bits<DEFLATE_MAX_PRE_CODEWORD_LEN + 7>();

            /* (The code below assumes that the precode decode table
             * does not have any subtables.)  */
            static_assert(PRECODE_TABLEBITS == DEFLATE_MAX_PRE_CODEWORD_LEN);

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
            static_assert(DEFLATE_MAX_LENS_OVERRUN == 138 - 1);

            if (presym == 16) {
                    /* Repeat the previous length 3 - 6 times  */
                    assert(i != 0);
                    const u8 rep_val = d->u.l.lens[i - 1];
                    static_assert(3 + ((1 << 2) - 1) == 6);
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
                    static_assert(3 + ((1 << 3) - 1) == 10);
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
                    static_assert(11 + ((1 << 7) - 1) == 138);
                    const unsigned rep_count = 11 + in_stream.pop_bits(7);
                    memset(&d->u.l.lens[i], 0,
                           rep_count * sizeof(d->u.l.lens[i]));
                    i += rep_count;
            }
    }

    assert(build_offset_decode_table(d, num_litlen_syms, num_offset_syms));
    assert(build_litlen_decode_table(d, num_litlen_syms, num_offset_syms));
}


void prepare_static(struct libdeflate_decompressor * restrict d) {
    /* Static Huffman block: set the static Huffman codeword
     * lengths.  Then the remainder is the same as decompressing a
     * dynamic Huffman block.  */
    for (unsigned i = 0; i < 144; i++)
            d->u.l.lens[i] = 8;
    for (unsigned i = 144; i < 256; i++)
            d->u.l.lens[i] = 9;
    for (unsigned i = 256; i < 280; i++)
            d->u.l.lens[i] = 7;
    for (unsigned i = 280; i < DEFLATE_NUM_LITLEN_SYMS; i++)
            d->u.l.lens[i] = 8;
    for (unsigned i = DEFLATE_NUM_LITLEN_SYMS; i < DEFLATE_NUM_LITLEN_SYMS + DEFLATE_NUM_OFFSET_SYMS; i++)
            d->u.l.lens[i] = 5;

    assert(build_offset_decode_table(d, DEFLATE_NUM_LITLEN_SYMS, DEFLATE_NUM_OFFSET_SYMS));
    assert(build_litlen_decode_table(d, DEFLATE_NUM_LITLEN_SYMS, DEFLATE_NUM_OFFSET_SYMS));
}


u16 do_uncompressed(InputStream& in_stream, byte* const out, const byte* const out_end) {
    /* Uncompressed block: copy 'len' bytes literally from the input
     * buffer to the output buffer.  */

    in_stream.align_input();

    assert(in_stream.size() >= 4);

    u16 len = in_stream.pop_u16();
    u16 nlen = in_stream.pop_u16();

    assert(len == (u16)~nlen);
    if (unlikely(len > out_end - out))
            throw DeflateException(LIBDEFLATE_INSUFFICIENT_SPACE);
    assert(len <= in_stream.size());

    in_stream.copy(out, len);
    return len;
}


unsigned copy_match(byte* const out_next, const byte  * const out_end, unsigned length, unsigned offset) {
    if (length <= (3 * WORDBYTES) &&
        offset >= WORDBYTES &&
        length + (3 * WORDBYTES) <= out_end - out_next)
    {
            /* Fast case: short length, no overlaps if we copy one
             * word at a time, and we aren't getting too close to
             * the end of the output array.  */
            copy_word_unaligned(out_next - offset + (0 * WORDBYTES),
                                out_next + (0 * WORDBYTES));
            copy_word_unaligned(out_next - offset + (1 * WORDBYTES),
                                out_next + (1 * WORDBYTES));
            copy_word_unaligned(out_next - offset + (2 * WORDBYTES),
                                out_next + (2 * WORDBYTES));
    } else {
            const byte *src = out_next - offset;
            byte *dst = out_next;
            byte *end = out_next + length;

            if (likely(out_end - end >= WORDBYTES - 1)) {
                    if (offset >= WORDBYTES) {
                            copy_word_unaligned(src, dst);
                            src += WORDBYTES;
                            dst += WORDBYTES;
                            if (dst < end) {
                                    do {
                                            copy_word_unaligned(src, dst);
                                            src += WORDBYTES;
                                            dst += WORDBYTES;
                                    } while (dst < end);
                            }
                    } else if (offset == 1) {
                            machine_word_t v = repeat_byte(*(dst - 1));
                            do {
                                    store_word_unaligned(v, dst);
                                    src += WORDBYTES;
                                    dst += WORDBYTES;
                            } while (dst < end);
                    } else {
                            *dst++ = *src++;
                            *dst++ = *src++;
                            do {
                                    *dst++ = *src++;
                            } while (dst < end);
                    }
            } else {
                    *dst++ = *src++;
                    *dst++ = *src++;
                    do {
                            *dst++ = *src++;
                    } while (dst < end);
            }
    }

    return length;
}

class OutBlock {
public:
    OutBlock(unsigned window_bits) : buffer(new byte[1 << window_bits]), buffer_end(buffer + (1 << window_bits)) {
        clear();
    }

    ~OutBlock() {
        delete[] buffer;
    }

    void clear() {
        next = buffer;
    }

    void copy_match(unsigned length, unsigned offset) {
        if (length <= (3 * WORDBYTES) &&
            offset >= WORDBYTES &&
            length + (3 * WORDBYTES) <= buffer_end - next)
        {
            /* Fast case: short length, no overlaps if we copy one
             * word at a time, and we aren't getting too close to
             * the end of the output array.  */
            copy_word_unaligned(next - offset + (0 * WORDBYTES),
                                next + (0 * WORDBYTES));
            copy_word_unaligned(next - offset + (1 * WORDBYTES),
                                next + (1 * WORDBYTES));
            copy_word_unaligned(next - offset + (2 * WORDBYTES),
                                next + (2 * WORDBYTES));
        } else {
            const byte *src = next - offset;
            byte *dst = next;
            const  byte* const dst_end = dst + length;

            if (likely(buffer_end - dst_end >= WORDBYTES - 1)) {
                if (offset >= WORDBYTES) {
                    copy_word_unaligned(src, dst);
                    src += WORDBYTES;
                    dst += WORDBYTES;
                    if (dst < dst_end) {
                        do {
                            copy_word_unaligned(src, dst);
                            src += WORDBYTES;
                            dst += WORDBYTES;
                        } while (dst < dst_end);
                    }
                } else if (offset == 1) {
                    machine_word_t v = repeat_byte(*(dst - 1));
                    do {
                        store_word_unaligned(v, dst);
                        src += WORDBYTES;
                        dst += WORDBYTES;
                    } while (dst < dst_end);
                } else {
                    *dst++ = *src++;
                    *dst++ = *src++;
                    do {
                        *dst++ = *src++;
                    } while (dst < dst_end);
                }
            } else {
                *dst++ = *src++;
                *dst++ = *src++;
                do {
                      *dst++ = *src++;
                } while (dst < dst_end);
            }
        }

        next += length;
    }

    void copy(InputStream & in, unsigned length) {
        in.copy(next, length);
        next += length;
    }

    //FIXME: debug only
    unsigned dump(byte* const dst) {
        memcpy(dst, buffer, next - buffer);
        return next - buffer;
    }

protected:
    byte* const buffer;
    const byte* const buffer_end; /// Past the end pointer
    byte* next; /// Next byte to be written
};


byte* do_block(struct libdeflate_decompressor * restrict d, InputStream& in_stream, byte* out_next, const byte * const out_end, const byte * const out, unsigned& is_final_block)
{
    /* Starting to read the next block.  */
    in_stream.ensure_bits<1 + 2 + 5 + 5 + 4>();

    /* BFINAL: 1 bit  */
    is_final_block = in_stream.pop_bits(1);

    /* BTYPE: 2 bits  */
    switch(in_stream.pop_bits(2)) {
    case DEFLATE_BLOCKTYPE_DYNAMIC_HUFFMAN:
        prepare_dynamic(d, in_stream);
        break;

    case DEFLATE_BLOCKTYPE_UNCOMPRESSED:
        out_next += do_uncompressed(in_stream, out_next, out_end);
        return out_next;
        break;

    case DEFLATE_BLOCKTYPE_STATIC_HUFFMAN:
        prepare_static(d);
        break;

    default:
        assert(false);
    }

    /* Decompressing a Huffman block (either dynamic or static)  */



    /* The main DEFLATE decode loop  */
    for (;;) {
            /* Decode a litlen symbol.  */
            in_stream.ensure_bits<DEFLATE_MAX_LITLEN_CODEWORD_LEN>();
            //FIXME: entry should be const
            u32 entry = d->u.litlen_decode_table[in_stream.bits(LITLEN_TABLEBITS)];
            if (entry & HUFFDEC_SUBTABLE_POINTER) {
                    /* Litlen subtable required (uncommon case)  */
                    in_stream.remove_bits(LITLEN_TABLEBITS);
                    entry = d->u.litlen_decode_table[
                            ((entry >> HUFFDEC_RESULT_SHIFT) & 0xFFFF) +
                            in_stream.bits(entry & HUFFDEC_LENGTH_MASK)];
            }
            in_stream.remove_bits(entry & HUFFDEC_LENGTH_MASK);
            if (entry & HUFFDEC_LITERAL) {
                    /* Literal  */
                    if (unlikely(out_next == out_end))
                            throw DeflateException(LIBDEFLATE_INSUFFICIENT_SPACE);
                    *out_next++ = byte(entry >> HUFFDEC_RESULT_SHIFT);
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
            static_assert(HUFFDEC_END_OF_BLOCK_LENGTH == 0);
            if (unlikely(size_t(length) - 1 >= size_t(out_end - out_next))) {
                    if (unlikely(length != HUFFDEC_END_OF_BLOCK_LENGTH))
                            throw DeflateException(LIBDEFLATE_INSUFFICIENT_SPACE);
                    return out_next;
            }

            /* Decode the match offset.  */
            entry = d->offset_decode_table[in_stream.bits(OFFSET_TABLEBITS)];
            if (entry & HUFFDEC_SUBTABLE_POINTER) {
                    /* Offset subtable required (uncommon case)  */
                    in_stream.remove_bits(OFFSET_TABLEBITS);
                    entry = d->offset_decode_table[
                            ((entry >> HUFFDEC_RESULT_SHIFT) & 0xFFFF) +
                            in_stream.bits(entry & HUFFDEC_LENGTH_MASK)];
            }
            in_stream.remove_bits(entry & HUFFDEC_LENGTH_MASK);
            entry >>= HUFFDEC_RESULT_SHIFT;

            /* Pop the extra offset bits and add them to the offset base to
             * produce the full offset.  */
            const u32 offset = (entry & HUFFDEC_OFFSET_BASE_MASK) +
                     in_stream.pop_bits(entry >> HUFFDEC_EXTRA_OFFSET_BITS_SHIFT);

            /* The match source must not begin before the beginning of the
             * output buffer.  */
            assert(offset <= out_next - out);

            /* Copy the match: 'length' bytes at 'out_next - offset' to
             * 'out_next'.  */
            out_next += copy_match(out_next, out_end, length, offset);
    }
    return out_next;
}

LIBDEFLATEAPI enum libdeflate_result
libdeflate_deflate_decompress(struct libdeflate_decompressor * restrict d,
			      const byte * restrict const in, size_t in_nbytes,
			      byte * restrict const out, size_t out_nbytes_avail,
			      size_t *actual_out_nbytes_ret)
{
        InputStream in_stream(in, in_nbytes);
        OutBlock out_block(15);
	byte *out_next = out;
	byte * const out_end = out_next + out_nbytes_avail;


        unsigned is_final_block;
next_block:
        out_next = do_block(d, in_stream, out_next, out_end, out, is_final_block);
        goto block_done;

block_done:
	/* Finished decoding a block.  */

	if (!is_final_block)
		goto next_block;

	/* That was the last block.  */

	if (actual_out_nbytes_ret) {
		*actual_out_nbytes_ret = out_next - out;
	} else {
		if (out_next != out_end)
			return LIBDEFLATE_SHORT_OUTPUT;
	}
	return LIBDEFLATE_SUCCESS;
}
