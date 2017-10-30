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
LIBDEFLATEAPI enum libdeflate_result
libdeflate_deflate_decompress(struct libdeflate_decompressor * restrict d,
			      const byte * restrict in, size_t in_nbytes,
			      byte * restrict out, size_t out_nbytes_avail,
			      size_t *actual_out_nbytes_ret)
{
        InputStream in_stream(in, in_nbytes);
	byte *out_next = out;
	byte * const out_end = out_next + out_nbytes_avail;


	unsigned num_litlen_syms;
	unsigned num_offset_syms;

next_block:
	/* Starting to read the next block.  */
	in_stream.ensure_bits<1 + 2 + 5 + 5 + 4>();

	/* BFINAL: 1 bit  */
	unsigned is_final_block = in_stream.pop_bits(1);

	/* BTYPE: 2 bits  */
	unsigned block_type = in_stream.pop_bits(2);

	if (block_type == DEFLATE_BLOCKTYPE_DYNAMIC_HUFFMAN) {

		/* Dynamic Huffman block.  */

		/* The order in which precode lengths are stored.  */
		static const u8 deflate_precode_lens_permutation[DEFLATE_NUM_PRECODE_SYMS] = {
			16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
		};

		unsigned num_explicit_precode_lens;

		/* Read the codeword length counts.  */

		static_assert(DEFLATE_NUM_LITLEN_SYMS == ((1 << 5) - 1) + 257);
		num_litlen_syms = in_stream.pop_bits(5) + 257;

		static_assert(DEFLATE_NUM_OFFSET_SYMS == ((1 << 5) - 1) + 1);
		num_offset_syms = in_stream.pop_bits(5) + 1;

		static_assert(DEFLATE_NUM_PRECODE_SYMS == ((1 << 4) - 1) + 4);
		num_explicit_precode_lens = in_stream.pop_bits(4) + 4;

		/* Read the precode codeword lengths.  */
		static_assert(DEFLATE_MAX_PRE_CODEWORD_LEN == (1 << 3) - 1);

                in_stream.ensure_bits<DEFLATE_NUM_PRECODE_SYMS * 3>();

                unsigned i = 0;
                for (; i < num_explicit_precode_lens; i++)
                        d->u.precode_lens[deflate_precode_lens_permutation[i]] = in_stream.pop_bits(3);

		for (; i < DEFLATE_NUM_PRECODE_SYMS; i++)
			d->u.precode_lens[deflate_precode_lens_permutation[i]] = 0;

		/* Build the decode table for the precode.  */
		SAFETY_CHECK(build_precode_decode_table(d));

		/* Expand the literal/length and offset codeword lengths.  */
		for (i = 0; i < num_litlen_syms + num_offset_syms; ) {
			u32 entry;
			unsigned presym;
			u8 rep_val;
			unsigned rep_count;

			in_stream.ensure_bits<DEFLATE_MAX_PRE_CODEWORD_LEN + 7>();

			/* (The code below assumes that the precode decode table
			 * does not have any subtables.)  */
			STATIC_ASSERT(PRECODE_TABLEBITS == DEFLATE_MAX_PRE_CODEWORD_LEN);

			/* Read the next precode symbol.  */
			entry = d->u.l.precode_decode_table[in_stream.bits(DEFLATE_MAX_PRE_CODEWORD_LEN)];
			in_stream.remove_bits(entry & HUFFDEC_LENGTH_MASK);
			presym = entry >> HUFFDEC_RESULT_SHIFT;

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
			STATIC_ASSERT(DEFLATE_MAX_LENS_OVERRUN == 138 - 1);

			if (presym == 16) {
				/* Repeat the previous length 3 - 6 times  */
				SAFETY_CHECK(i != 0);
				rep_val = d->u.l.lens[i - 1];
				STATIC_ASSERT(3 + ((1 << 2) - 1) == 6);
				rep_count = 3 + in_stream.pop_bits(2);
				d->u.l.lens[i + 0] = rep_val;
				d->u.l.lens[i + 1] = rep_val;
				d->u.l.lens[i + 2] = rep_val;
				d->u.l.lens[i + 3] = rep_val;
				d->u.l.lens[i + 4] = rep_val;
				d->u.l.lens[i + 5] = rep_val;
				i += rep_count;
			} else if (presym == 17) {
				/* Repeat zero 3 - 10 times  */
				STATIC_ASSERT(3 + ((1 << 3) - 1) == 10);
				rep_count = 3 + in_stream.pop_bits(3);
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
				STATIC_ASSERT(11 + ((1 << 7) - 1) == 138);
				rep_count = 11 + in_stream.pop_bits(7);
				memset(&d->u.l.lens[i], 0,
				       rep_count * sizeof(d->u.l.lens[i]));
				i += rep_count;
			}
		}
	} else if (block_type == DEFLATE_BLOCKTYPE_UNCOMPRESSED) {

		/* Uncompressed block: copy 'len' bytes literally from the input
		 * buffer to the output buffer.  */

		in_stream.align_input();

		SAFETY_CHECK(in_stream.size() >= 4);

		u16 len = in_stream.pop_u16();
		u16 nlen = in_stream.pop_u16();

		SAFETY_CHECK(len == (u16)~nlen);
		if (unlikely(len > out_end - out_next))
			return LIBDEFLATE_INSUFFICIENT_SPACE;
		SAFETY_CHECK(len <= in_stream.size());

                in_stream.copy(out_next, len);
		out_next += len;

		goto block_done;

	} else {
		SAFETY_CHECK(block_type == DEFLATE_BLOCKTYPE_STATIC_HUFFMAN);

		/* Static Huffman block: set the static Huffman codeword
		 * lengths.  Then the remainder is the same as decompressing a
		 * dynamic Huffman block.  */

		STATIC_ASSERT(DEFLATE_NUM_LITLEN_SYMS == 288);
		STATIC_ASSERT(DEFLATE_NUM_OFFSET_SYMS == 32);

        unsigned i = 0;
		for (; i < 144; i++)
			d->u.l.lens[i] = 8;
		for (; i < 256; i++)
			d->u.l.lens[i] = 9;
		for (; i < 280; i++)
			d->u.l.lens[i] = 7;
		for (; i < 288; i++)
			d->u.l.lens[i] = 8;

		for (; i < 288 + 32; i++)
			d->u.l.lens[i] = 5;

		num_litlen_syms = 288;
		num_offset_syms = 32;

	}

	/* Decompressing a Huffman block (either dynamic or static)  */

	SAFETY_CHECK(build_offset_decode_table(d, num_litlen_syms, num_offset_syms));
	SAFETY_CHECK(build_litlen_decode_table(d, num_litlen_syms, num_offset_syms));

	/* The main DEFLATE decode loop  */
	for (;;) {
		u32 entry;
		u32 length;
		u32 offset;

		/* Decode a litlen symbol.  */
		in_stream.ensure_bits<DEFLATE_MAX_LITLEN_CODEWORD_LEN>();
		entry = d->u.litlen_decode_table[in_stream.bits(LITLEN_TABLEBITS)];
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
				return LIBDEFLATE_INSUFFICIENT_SPACE;
			*out_next++ = byte(entry >> HUFFDEC_RESULT_SHIFT);
			continue;
		}

		/* Match or end-of-block  */

		entry >>= HUFFDEC_RESULT_SHIFT;
		in_stream.ensure_bits<in_stream.bitbuf_max_ensure>();

		/* Pop the extra length bits and add them to the length base to
		 * produce the full length.  */
		length = (entry >> HUFFDEC_LENGTH_BASE_SHIFT) +
			 in_stream.pop_bits(entry & HUFFDEC_EXTRA_LENGTH_BITS_MASK);

		/* The match destination must not end after the end of the
		 * output buffer.  For efficiency, combine this check with the
		 * end-of-block check.  We're using 0 for the special
		 * end-of-block length, so subtract 1 and it turn it into
		 * SIZE_MAX.  */
		STATIC_ASSERT(HUFFDEC_END_OF_BLOCK_LENGTH == 0);
		if (unlikely(size_t(length) - 1 >= size_t(out_end - out_next))) {
			if (unlikely(length != HUFFDEC_END_OF_BLOCK_LENGTH))
				return LIBDEFLATE_INSUFFICIENT_SPACE;
			goto block_done;
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
		offset = (entry & HUFFDEC_OFFSET_BASE_MASK) +
			 in_stream.pop_bits(entry >> HUFFDEC_EXTRA_OFFSET_BITS_SHIFT);

		/* The match source must not begin before the beginning of the
		 * output buffer.  */
		SAFETY_CHECK(offset <= out_next - out);

		/* Copy the match: 'length' bytes at 'out_next - offset' to
		 * 'out_next'.  */

		if (UNALIGNED_ACCESS_IS_FAST &&
		    length <= (3 * WORDBYTES) &&
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

			if (UNALIGNED_ACCESS_IS_FAST &&
			    likely(out_end - end >= WORDBYTES - 1)) {
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

		out_next += length;
	}

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
