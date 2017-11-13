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

#include <stdlib.h>
#include <string.h>

#include "deflate_constants.h"
#include "unaligned.h"
//#include "x86_cpu_features.h"

#include "libdeflate.h"

/*
 * If the expression passed to SAFETY_CHECK() evaluates to false, then the
 * decompression routine immediately returns LIBDEFLATE_BAD_DATA, indicating the
 * compressed data is invalid.
 *
 * Theoretically, these checks could be disabled for specialized applications
 * where all input to the decompressor will be trusted.
 */
#if 0
#  pragma message("UNSAFE DECOMPRESSION IS ENABLED. THIS MUST ONLY BE USED IF THE DECOMPRESSOR INPUT WILL ALWAYS BE TRUSTED!")
#  define SAFETY_CHECK(expr)	(void)(expr)
#else
#  define SAFETY_CHECK(expr)	if (unlikely(!(expr))) return LIBDEFLATE_BAD_DATA
#endif

/*
 * Each TABLEBITS number is the base-2 logarithm of the number of entries in the
 * main portion of the corresponding decode table.  Each number should be large
 * enough to ensure that for typical data, the vast majority of symbols can be
 * decoded by a direct lookup of the next TABLEBITS bits of compressed data.
 * However, this must be balanced against the fact that a larger table requires
 * more memory and requires more time to fill.
 *
 * Note: you cannot change a TABLEBITS number without also changing the
 * corresponding ENOUGH number!
 */
#define PRECODE_TABLEBITS	7
#define LITLEN_TABLEBITS	10
#define OFFSET_TABLEBITS	8

/*
 * Each ENOUGH number is the maximum number of decode table entries that may be
 * required for the corresponding Huffman code, including the main table and all
 * subtables.  Each number depends on three parameters:
 *
 *	(1) the maximum number of symbols in the code (DEFLATE_NUM_*_SYMBOLS)
 *	(2) the number of main table bits (the TABLEBITS numbers defined above)
 *	(3) the maximum allowed codeword length (DEFLATE_MAX_*_CODEWORD_LEN)
 *
 * The ENOUGH numbers were computed using the utility program 'enough' from
 * zlib.  This program enumerates all possible relevant Huffman codes to find
 * the worst-case usage of decode table entries.
 */
#define PRECODE_ENOUGH		128	/* enough 19 7 7	*/
#define LITLEN_ENOUGH		1334	/* enough 288 10 15	*/
#define OFFSET_ENOUGH		402	/* enough 32 8 15	*/

/*
 * Type for codeword lengths.
 */
typedef u8 len_t;

/*
 * The main DEFLATE decompressor structure.  Since this implementation only
 * supports full buffer decompression, this structure does not store the entire
 * decompression state, but rather only some arrays that are too large to
 * comfortably allocate on the stack.
 */
struct libdeflate_decompressor {

	/*
	 * The arrays aren't all needed at the same time.  'precode_lens' and
	 * 'precode_decode_table' are unneeded after 'lens' has been filled.
	 * Furthermore, 'lens' need not be retained after building the litlen
	 * and offset decode tables.  In fact, 'lens' can be in union with
	 * 'litlen_decode_table' provided that 'offset_decode_table' is separate
	 * and is built first.
	 */

	union {
		len_t precode_lens[DEFLATE_NUM_PRECODE_SYMS];

		struct {
			len_t lens[DEFLATE_NUM_LITLEN_SYMS +
				   DEFLATE_NUM_OFFSET_SYMS +
				   DEFLATE_MAX_LENS_OVERRUN];

			u32 precode_decode_table[PRECODE_ENOUGH];
		} l;

		u32 litlen_decode_table[LITLEN_ENOUGH];
	} u;

	u32 offset_decode_table[OFFSET_ENOUGH];

	u16 working_space[2 * (DEFLATE_MAX_CODEWORD_LEN + 1) +
			  DEFLATE_MAX_NUM_SYMS];
};

/*****************************************************************************
 *				Input bitstream                              *
 *****************************************************************************/

/*
 * The state of the "input bitstream" consists of the following variables:
 *
 *	- in_next: pointer to the next unread byte in the input buffer
 *
 *	- in_end: pointer just past the end of the input buffer
 *
 *	- bitbuf: a word-sized variable containing bits that have been read from
 *		  the input buffer.  The buffered bits are right-aligned
 *		  (they're the low-order bits).
 *
 *	- bitsleft: number of bits in 'bitbuf' that are valid.
 *
 * To make it easier for the compiler to optimize the code by keeping variables
 * in registers, these are declared as normal variables and manipulated using
 * macros.
 */

/*
 * The type for the bitbuffer variable ('bitbuf' described above).  For best
 * performance, this should have size equal to a machine word.
 *
 * 64-bit platforms have a significant advantage: they get a bigger bitbuffer
 * which they have to fill less often.
 */
typedef machine_word_t bitbuf_t;



#include <cstdio>

#undef NDEBUG //FIXME: forced debug

#ifdef NDEBUG
#define assert(expr)							\
 (likely((expr))							\
  ? static_cast<void>(0)						\
  : __builtin_unreachable())
#else
#define assert(expr)							\
 (likely((expr))							\
  ? static_cast<void>(0)						\
  : __assert_fail (#expr, __FILE__, __LINE__, __PRETTY_FUNCTION__))
#endif

[[noreturn]] inline void
__assert_fail (const char *assertion, const char *file, unsigned int line, const char *function) noexcept {
    std::fprintf(stderr, "%s:%u: Assertion '%s' failed in '%s'.\n",
                 file, line, assertion, function);
    std::fflush(stderr);
    std::abort();
}


#include <algorithm>

class InputStream {

protected:
    /** State */
    bitbuf_t bitbuf; /// Bit buffer
    size_t bitsleft; /// Number of valid bits in the bit buffer
    size_t overrun_count;
    const byte * restrict in_next; /// Read pointer
    const byte * restrict const in_end; /// Adress of the byte after input

    /**
     * Fill the bitbuffer variable by reading the next word from the input buffer.
     * This can be significantly faster than FILL_BITS_BYTEWISE().  However, for
     * this to work correctly, the word must be interpreted in little-endian format.
     * In addition, the memory access may be unaligned.  Therefore, this method is
     * most efficient on little-endian architectures that support fast unaligned
     * access, such as x86 and x86_64.
     */
    inline void fill_bits_wordwise() {
        bitbuf |= get_unaligned_leword(in_next) << bitsleft;
        in_next += (bitbuf_length - bitsleft) >> 3;
        bitsleft += (bitbuf_length - bitsleft) & ~7;
    }

    /**
     * Does the bitbuffer variable currently contain at least 'n' bits?
     */
    constexpr bool have_bits(size_t n) const
    { return bitsleft >= n; }

    /**
     * Fill the bitbuffer variable, reading one byte at a time.
     *
     * Note: if we would overrun the input buffer, we just don't read anything,
     * leaving the bits as 0 but marking them as filled.  This makes the
     * implementation simpler because this removes the need to distinguish between
     * "real" overruns and overruns that occur because of our own lookahead during
     * Huffman decoding.  The disadvantage is that a "real" overrun can go
     * undetected, and libdeflate_deflate_decompress() may return a success status
     * rather than the expected failure status if one occurs.  However, this is
     * irrelevant because even if this specific case were to be handled "correctly",
     * one could easily come up with a different case where the compressed data
     * would be corrupted in such a way that fully retains its validity.  Users
     * should run a checksum against the uncompressed data if they wish to detect
     * corruptions.
     */
    inline void fill_bits_bytewise() {
        do {
               if (likely(in_next != in_end))
                       bitbuf |= (bitbuf_t)*in_next++ << bitsleft;
               else
                       overrun_count++;
               bitsleft += 8;
        } while (bitsleft <= bitbuf_length - 8);
    }

public:
    InputStream(const byte* in, size_t len) : bitbuf(0), bitsleft(0), overrun_count(0),
        in_next(in), in_end(in + len)
    {}

    /**
     * Number of bits the bitbuffer variable can hold.
     */
    static constexpr size_t bitbuf_length = 8 * sizeof(bitbuf_t);

    /**
     * The maximum number of bits that can be requested to be in the bitbuffer
     * variable.  This is the maximum value of 'n' that can be passed
     * ENSURE_BITS(n).
     *
     * This not equal to BITBUF_NBITS because we never read less than one byte at a
     * time.  If the bitbuffer variable contains more than (BITBUF_NBITS - 8) bits,
     * then we can't read another byte without first consuming some bits.  So the
     * maximum count we can ensure is (BITBUF_NBITS - 7).
     */
    static constexpr size_t bitbuf_max_ensure = bitbuf_length - 7;

    constexpr size_t size() const {
        return in_end - in_end;
    }

    /**
     * Load more bits from the input buffer until the specified number of bits is
     * present in the bitbuffer variable.  'n' cannot be too large; see MAX_ENSURE
     * and CAN_ENSURE().
     */
    template <size_t n>
    inline void ensure_bits() {
        static_assert(n <= bitbuf_max_ensure, "Bit buffer is too small");
        if (!have_bits(n)) {						\
                if (likely(in_end - in_next >= static_cast<std::ptrdiff_t>(sizeof(bitbuf_t))))	\
                        fill_bits_wordwise();				\
                else fill_bits_bytewise();                              \
        }
    }

    /**
     * Return the next 'n' bits from the bitbuffer variable without removing them.
     */
    constexpr u32 bits(size_t n) const
    {
        assert(bitsleft >= n);
        return  u32(bitbuf & ((u32(1) << n) - 1));
    }

    /**
     * Remove the next 'n' bits from the bitbuffer variable.
     */
    inline void remove_bits(size_t n) {
        assert(bitsleft >= n);
        bitbuf >>= n;
        bitsleft -= n;
    }

    /**
     * Remove and return the next 'n' bits from the bitbuffer variable.
     */
    inline u32 pop_bits(size_t n) {
        u32 tmp = bits(n);
        remove_bits(n);
        return tmp;
    }

    /**
     * Align the input to the next byte boundary, discarding any remaining bits in
     * the current byte.
     *
     * Note that if the bitbuffer variable currently contains more than 8 bits, then
     * we must rewind 'in_next', effectively putting those bits back.  Only the bits
     * in what would be the "current" byte if we were reading one byte at a time can
     * be actually discarded.
     */
    inline void align_input() {
        in_next -= (bitsleft >> 3) - std::min(overrun_count, bitsleft >> 3);
        bitbuf = 0;
        bitsleft = 0;
    }

    /**
     * Read a 16-bit value from the input.  This must have been preceded by a call
     * to ALIGN_INPUT(), and the caller must have already checked for overrun.
     */
    inline u16 pop_u16() {
        assert(size() >= 2);
        u16 tmp = get_unaligned_le16(in_next);
        in_next += 2;
        return tmp;
    }


    /**
      * Copy n bytes to the ouput buffer. The input beffer must be aligned with a
      * call to align_input()
      */
    inline void copy(byte* restrict out, size_t n) {
        assert(size() >= n);
        memcpy(out, in_next, n);
        in_next += n;
    }

};


/*****************************************************************************
 *                              Huffman decoding                             *
 *****************************************************************************/

/*
 * A decode table for order TABLEBITS consists of a main table of (1 <<
 * TABLEBITS) entries followed by a variable number of subtables.
 *
 * The decoding algorithm takes the next TABLEBITS bits of compressed data and
 * uses them as an index into the decode table.  The resulting entry is either a
 * "direct entry", meaning that it contains the value desired, or a "subtable
 * pointer", meaning that the entry references a subtable that must be indexed
 * using more bits of the compressed data to decode the symbol.
 *
 * Each decode table (a main table along with with its subtables, if any) is
 * associated with a Huffman code.  Logically, the result of a decode table
 * lookup is a symbol from the alphabet from which the corresponding Huffman
 * code was constructed.  A symbol with codeword length n <= TABLEBITS is
 * associated with 2**(TABLEBITS - n) direct entries in the table, whereas a
 * symbol with codeword length n > TABLEBITS is associated with one or more
 * subtable entries.
 *
 * On top of this basic design, we implement several optimizations:
 *
 * - We store the length of each codeword directly in each of its decode table
 *   entries.  This allows the codeword length to be produced without indexing
 *   an additional table.
 *
 * - When beneficial, we don't store the Huffman symbol itself, but instead data
 *   generated from it.  For example, when decoding an offset symbol in DEFLATE,
 *   it's more efficient if we can decode the offset base and number of extra
 *   offset bits directly rather than decoding the offset symbol and then
 *   looking up both of those values in an additional table or tables.
 *
 * The size of each decode table entry is 32 bits, which provides slightly
 * better performance than 16-bit entries on 32 and 64 bit processers, provided
 * that the table doesn't get so large that it takes up too much memory and
 * starts generating cache misses.  The bits of each decode table entry are
 * defined as follows:
 *
 * - Bits 30 -- 31: flags (see below)
 * - Bits 8 -- 29: decode result: a Huffman symbol or related data
 * - Bits 0 -- 7: codeword length
 */

namespace table_builder {



/*
 * This flag is set in all main decode table entries that represent subtable
 * pointers.
 */
constexpr u32 HUFFDEC_SUBTABLE_POINTER = 0x80000000;

/*
 * This flag is set in all entries in the litlen decode table that represent
 * literals.
 */
constexpr u32  HUFFDEC_LITERAL = 0x40000000;

/* Mask for extracting the codeword length from a decode table entry.  */
constexpr u32 HUFFDEC_LENGTH_MASK = 0xFF;

/* Shift to extract the decode result from a decode table entry.  */
constexpr size_t HUFFDEC_RESULT_SHIFT = 8;

/* The decode result for each precode symbol.  There is no special optimization
 * for the precode; the decode result is simply the symbol value.  */
static constexpr u32 precode_decode_results[DEFLATE_NUM_PRECODE_SYMS] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
};


constexpr u32 literal_entry(u32 literal) {
    return (HUFFDEC_LITERAL >> HUFFDEC_RESULT_SHIFT) | literal;
}

constexpr u32 HUFFDEC_EXTRA_LENGTH_BITS_MASK = 0xFF;
constexpr size_t HUFFDEC_LENGTH_BASE_SHIFT = 8;
constexpr u32 HUFFDEC_END_OF_BLOCK_LENGTH = 0;

constexpr u32 length_entry(u32 length_base, u32 num_extra_bits) {
    return (length_base << HUFFDEC_LENGTH_BASE_SHIFT) | num_extra_bits;
}






/* The decode result for each litlen symbol.  For literals, this is the literal
 * value itself and the HUFFDEC_LITERAL flag.  For lengths, this is the length
 * base and the number of extra length bits.  */
static constexpr u32 litlen_decode_results[DEFLATE_NUM_LITLEN_SYMS] = {

	/* Literals  */
	literal_entry(0)   , literal_entry(1)   , literal_entry(2)   , literal_entry(3)   ,
	literal_entry(4)   , literal_entry(5)   , literal_entry(6)   , literal_entry(7)   ,
	literal_entry(8)   , literal_entry(9)   , literal_entry(10)  , literal_entry(11)  ,
	literal_entry(12)  , literal_entry(13)  , literal_entry(14)  , literal_entry(15)  ,
	literal_entry(16)  , literal_entry(17)  , literal_entry(18)  , literal_entry(19)  ,
	literal_entry(20)  , literal_entry(21)  , literal_entry(22)  , literal_entry(23)  ,
	literal_entry(24)  , literal_entry(25)  , literal_entry(26)  , literal_entry(27)  ,
	literal_entry(28)  , literal_entry(29)  , literal_entry(30)  , literal_entry(31)  ,
	literal_entry(32)  , literal_entry(33)  , literal_entry(34)  , literal_entry(35)  ,
	literal_entry(36)  , literal_entry(37)  , literal_entry(38)  , literal_entry(39)  ,
	literal_entry(40)  , literal_entry(41)  , literal_entry(42)  , literal_entry(43)  ,
	literal_entry(44)  , literal_entry(45)  , literal_entry(46)  , literal_entry(47)  ,
	literal_entry(48)  , literal_entry(49)  , literal_entry(50)  , literal_entry(51)  ,
	literal_entry(52)  , literal_entry(53)  , literal_entry(54)  , literal_entry(55)  ,
	literal_entry(56)  , literal_entry(57)  , literal_entry(58)  , literal_entry(59)  ,
	literal_entry(60)  , literal_entry(61)  , literal_entry(62)  , literal_entry(63)  ,
	literal_entry(64)  , literal_entry(65)  , literal_entry(66)  , literal_entry(67)  ,
	literal_entry(68)  , literal_entry(69)  , literal_entry(70)  , literal_entry(71)  ,
	literal_entry(72)  , literal_entry(73)  , literal_entry(74)  , literal_entry(75)  ,
	literal_entry(76)  , literal_entry(77)  , literal_entry(78)  , literal_entry(79)  ,
	literal_entry(80)  , literal_entry(81)  , literal_entry(82)  , literal_entry(83)  ,
	literal_entry(84)  , literal_entry(85)  , literal_entry(86)  , literal_entry(87)  ,
	literal_entry(88)  , literal_entry(89)  , literal_entry(90)  , literal_entry(91)  ,
	literal_entry(92)  , literal_entry(93)  , literal_entry(94)  , literal_entry(95)  ,
	literal_entry(96)  , literal_entry(97)  , literal_entry(98)  , literal_entry(99)  ,
	literal_entry(100) , literal_entry(101) , literal_entry(102) , literal_entry(103) ,
	literal_entry(104) , literal_entry(105) , literal_entry(106) , literal_entry(107) ,
	literal_entry(108) , literal_entry(109) , literal_entry(110) , literal_entry(111) ,
	literal_entry(112) , literal_entry(113) , literal_entry(114) , literal_entry(115) ,
	literal_entry(116) , literal_entry(117) , literal_entry(118) , literal_entry(119) ,
	literal_entry(120) , literal_entry(121) , literal_entry(122) , literal_entry(123) ,
	literal_entry(124) , literal_entry(125) , literal_entry(126) , literal_entry(127) ,
	literal_entry(128) , literal_entry(129) , literal_entry(130) , literal_entry(131) ,
	literal_entry(132) , literal_entry(133) , literal_entry(134) , literal_entry(135) ,
	literal_entry(136) , literal_entry(137) , literal_entry(138) , literal_entry(139) ,
	literal_entry(140) , literal_entry(141) , literal_entry(142) , literal_entry(143) ,
	literal_entry(144) , literal_entry(145) , literal_entry(146) , literal_entry(147) ,
	literal_entry(148) , literal_entry(149) , literal_entry(150) , literal_entry(151) ,
	literal_entry(152) , literal_entry(153) , literal_entry(154) , literal_entry(155) ,
	literal_entry(156) , literal_entry(157) , literal_entry(158) , literal_entry(159) ,
	literal_entry(160) , literal_entry(161) , literal_entry(162) , literal_entry(163) ,
	literal_entry(164) , literal_entry(165) , literal_entry(166) , literal_entry(167) ,
	literal_entry(168) , literal_entry(169) , literal_entry(170) , literal_entry(171) ,
	literal_entry(172) , literal_entry(173) , literal_entry(174) , literal_entry(175) ,
	literal_entry(176) , literal_entry(177) , literal_entry(178) , literal_entry(179) ,
	literal_entry(180) , literal_entry(181) , literal_entry(182) , literal_entry(183) ,
	literal_entry(184) , literal_entry(185) , literal_entry(186) , literal_entry(187) ,
	literal_entry(188) , literal_entry(189) , literal_entry(190) , literal_entry(191) ,
	literal_entry(192) , literal_entry(193) , literal_entry(194) , literal_entry(195) ,
	literal_entry(196) , literal_entry(197) , literal_entry(198) , literal_entry(199) ,
	literal_entry(200) , literal_entry(201) , literal_entry(202) , literal_entry(203) ,
	literal_entry(204) , literal_entry(205) , literal_entry(206) , literal_entry(207) ,
	literal_entry(208) , literal_entry(209) , literal_entry(210) , literal_entry(211) ,
	literal_entry(212) , literal_entry(213) , literal_entry(214) , literal_entry(215) ,
	literal_entry(216) , literal_entry(217) , literal_entry(218) , literal_entry(219) ,
	literal_entry(220) , literal_entry(221) , literal_entry(222) , literal_entry(223) ,
	literal_entry(224) , literal_entry(225) , literal_entry(226) , literal_entry(227) ,
	literal_entry(228) , literal_entry(229) , literal_entry(230) , literal_entry(231) ,
	literal_entry(232) , literal_entry(233) , literal_entry(234) , literal_entry(235) ,
	literal_entry(236) , literal_entry(237) , literal_entry(238) , literal_entry(239) ,
	literal_entry(240) , literal_entry(241) , literal_entry(242) , literal_entry(243) ,
	literal_entry(244) , literal_entry(245) , literal_entry(246) , literal_entry(247) ,
	literal_entry(248) , literal_entry(249) , literal_entry(250) , literal_entry(251) ,
	literal_entry(252) , literal_entry(253) , literal_entry(254) , literal_entry(255) ,



	/* End of block  */
	length_entry(HUFFDEC_END_OF_BLOCK_LENGTH, 0),

	/* Lengths  */
	length_entry(3  , 0) , length_entry(4  , 0) , length_entry(5  , 0) , length_entry(6  , 0),
	length_entry(7  , 0) , length_entry(8  , 0) , length_entry(9  , 0) , length_entry(10 , 0),
	length_entry(11 , 1) , length_entry(13 , 1) , length_entry(15 , 1) , length_entry(17 , 1),
	length_entry(19 , 2) , length_entry(23 , 2) , length_entry(27 , 2) , length_entry(31 , 2),
	length_entry(35 , 3) , length_entry(43 , 3) , length_entry(51 , 3) , length_entry(59 , 3),
	length_entry(67 , 4) , length_entry(83 , 4) , length_entry(99 , 4) , length_entry(115, 4),
	length_entry(131, 5) , length_entry(163, 5) , length_entry(195, 5) , length_entry(227, 5),
	length_entry(258, 0) , length_entry(258, 0) , length_entry(258, 0) ,

};


constexpr size_t HUFFDEC_EXTRA_OFFSET_BITS_SHIFT = 16;
constexpr u32 HUFFDEC_OFFSET_BASE_MASK = (1 << HUFFDEC_EXTRA_OFFSET_BITS_SHIFT) - 1;

constexpr u32 offset_entry(u32 offset_base, u32 num_extra_bits) {
    return offset_base | (num_extra_bits << HUFFDEC_EXTRA_OFFSET_BITS_SHIFT);
}

/* The decode result for each offset symbol.  This is the offset base and the
 * number of extra offset bits.  */
static constexpr u32 offset_decode_results[DEFLATE_NUM_OFFSET_SYMS] = {
    offset_entry(1     , 0)  , offset_entry(2     , 0)  , offset_entry(3     , 0)  , offset_entry(4     , 0)  ,
    offset_entry(5     , 1)  , offset_entry(7     , 1)  , offset_entry(9     , 2)  , offset_entry(13    , 2) ,
    offset_entry(17    , 3)  , offset_entry(25    , 3)  , offset_entry(33    , 4)  , offset_entry(49    , 4)  ,
    offset_entry(65    , 5)  , offset_entry(97    , 5)  , offset_entry(129   , 6)  , offset_entry(193   , 6)  ,
    offset_entry(257   , 7)  , offset_entry(385   , 7)  , offset_entry(513   , 8)  , offset_entry(769   , 8)  ,
    offset_entry(1025  , 9)  , offset_entry(1537  , 9)  , offset_entry(2049  , 10) , offset_entry(3073  , 10) ,
    offset_entry(4097  , 11) , offset_entry(6145  , 11) , offset_entry(8193  , 12) , offset_entry(12289 , 12) ,
    offset_entry(16385 , 13) , offset_entry(24577 , 13) , offset_entry(32769 , 14) , offset_entry(49153 , 14) ,
};

/* Construct a decode table entry from a decode result and codeword length.  */
static forceinline u32
make_decode_table_entry(u32 result, u32 length)
{
	return (result << HUFFDEC_RESULT_SHIFT) | length;
}

/*
 * Build a table for fast decoding of symbols from a Huffman code.  As input,
 * this function takes the codeword length of each symbol which may be used in
 * the code.  As output, it produces a decode table for the canonical Huffman
 * code described by the codeword lengths.  The decode table is built with the
 * assumption that it will be indexed with "bit-reversed" codewords, where the
 * low-order bit is the first bit of the codeword.  This format is used for all
 * Huffman codes in DEFLATE.
 *
 * @decode_table
 *	The array in which the decode table will be generated.  This array must
 *	have sufficient length; see the definition of the ENOUGH numbers.
 * @lens
 *	An array which provides, for each symbol, the length of the
 *	corresponding codeword in bits, or 0 if the symbol is unused.  This may
 *	alias @decode_table, since nothing is written to @decode_table until all
 *	@lens have been consumed.  All codeword lengths are assumed to be <=
 *	@max_codeword_len but are otherwise considered untrusted.  If they do
 *	not form a valid Huffman code, then the decode table is not built and
 *	%false is returned.
 * @num_syms
 *	The number of symbols in the code, including all unused symbols.
 * @decode_results
 *	An array which provides, for each symbol, the actual value to store into
 *	the decode table.  This value will be directly produced as the result of
 *	decoding that symbol, thereby moving the indirection out of the decode
 *	loop and into the table initialization.
 * @table_bits
 *	The log base-2 of the number of main table entries to use.
 * @max_codeword_len
 *	The maximum allowed codeword length for this Huffman code.
 * @working_space
 *	A temporary array of length '2 * (@max_codeword_len + 1) + @num_syms'.
 *
 * Returns %true if successful; %false if the codeword lengths do not form a
 * valid Huffman code.
 */
static bool
build_decode_table(u32 decode_table[],
		   const len_t lens[],
		   const unsigned num_syms,
		   const u32 decode_results[],
		   const unsigned table_bits,
		   const unsigned max_codeword_len,
		   u16 working_space[])
{
	/* Count how many symbols have each codeword length, including 0.  */
        u16 * const len_counts = &working_space[0];
	for (unsigned len = 0; len <= max_codeword_len; len++)
		len_counts[len] = 0;
	for (unsigned sym = 0; sym < num_syms; sym++)
		len_counts[lens[sym]]++;

	/* Sort the symbols primarily by increasing codeword length and
	 * secondarily by increasing symbol value.  */

	/* Initialize 'offsets' so that offsets[len] is the number of codewords
	 * shorter than 'len' bits, including length 0.  */
        u16 * const offsets = &working_space[1 * (max_codeword_len + 1)];
	offsets[0] = 0;
	for (unsigned len = 0; len < max_codeword_len; len++)
		offsets[len + 1] = offsets[len] + len_counts[len];

	/* Use the 'offsets' array to sort the symbols.  */
        u16 * const sorted_syms = &working_space[2 * (max_codeword_len + 1)];
	for (unsigned sym = 0; sym < num_syms; sym++)
		sorted_syms[offsets[lens[sym]]++] = sym;

	/* It is already guaranteed that all lengths are <= max_codeword_len,
	 * but it cannot be assumed they form a complete prefix code.  A
	 * codeword of length n should require a proportion of the codespace
	 * equaling (1/2)^n.  The code is complete if and only if, by this
	 * measure, the codespace is exactly filled by the lengths.  */
	s32 remainder = 1;
	for (unsigned len = 1; len <= max_codeword_len; len++) {
		remainder <<= 1;
		remainder -= len_counts[len];
		if (unlikely(remainder < 0)) {
			/* The lengths overflow the codespace; that is, the code
			 * is over-subscribed.  */
			return false;
		}
	}

	if (unlikely(remainder != 0)) {
		/* The lengths do not fill the codespace; that is, they form an
		 * incomplete code.  */

		/* Initialize the table entries to default values.  When
		 * decompressing a well-formed stream, these default values will
		 * never be used.  But since a malformed stream might contain
		 * any bits at all, these entries need to be set anyway.  */
		u32 entry = make_decode_table_entry(decode_results[0], 1);
		for (unsigned sym = 0; sym < (1U << table_bits); sym++)
			decode_table[sym] = entry;

		/* A completely empty code is permitted.  */
		if (remainder == s32(1U << max_codeword_len))
			return true;

		/* The code is nonempty and incomplete.  Proceed only if there
		 * is a single used symbol and its codeword has length 1.  The
		 * DEFLATE RFC is somewhat unclear regarding this case.  What
		 * zlib's decompressor does is permit this case for
		 * literal/length and offset codes and assume the codeword is 0
		 * rather than 1.  We do the same except we allow this case for
		 * precodes too.  */
		if (remainder != s32(1U << (max_codeword_len - 1)) ||
		    len_counts[1] != 1)
			return false;
	}

	/* Generate the decode table entries.  Since we process codewords from
	 * shortest to longest, the main portion of the decode table is filled
	 * first; then the subtables are filled.  Note that it's already been
	 * verified that the code is nonempty and not over-subscribed.  */

	/* Start with the smallest codeword length and the smallest-valued
	 * symbol which has that codeword length.  */

	unsigned codeword_len = 1;
	while (len_counts[codeword_len] == 0)
		codeword_len++;

        unsigned codeword_reversed = 0;
	unsigned cur_codeword_prefix = -1;
	unsigned cur_table_start = 0;
	unsigned cur_table_bits = table_bits;
	unsigned num_dropped_bits = 0;
        unsigned sym_idx = offsets[0];
	const unsigned table_mask = (1U << table_bits) - 1;


	for (;;) {  /* For each used symbol and its codeword...  */
		/* Get the next symbol.  */
		unsigned sym = sorted_syms[sym_idx];

		/* Start a new subtable if the codeword is long enough to
		 * require a subtable, *and* the first 'table_bits' bits of the
		 * codeword don't match the prefix for the previous subtable if
		 * any.  */
		if (codeword_len > table_bits &&
		    (codeword_reversed & table_mask) != cur_codeword_prefix) {
			cur_codeword_prefix = (codeword_reversed & table_mask);

			cur_table_start += 1U << cur_table_bits;

			/* Calculate the subtable length.  If the codeword
			 * length exceeds 'table_bits' by n, the subtable needs
			 * at least 2**n entries.  But it may need more; if
			 * there are fewer than 2**n codewords of length
			 * 'table_bits + n' remaining, then n will need to be
			 * incremented to bring in longer codewords until the
			 * subtable can be filled completely.  Note that it
			 * always will, eventually, be possible to fill the
			 * subtable, since the only case where we may have an
			 * incomplete code is a single codeword of length 1,
			 * and that never requires any subtables.  */
			cur_table_bits = codeword_len - table_bits;
			remainder = (s32)1 << cur_table_bits;
			for (;;) {
				remainder -= len_counts[table_bits +
							cur_table_bits];
				if (remainder <= 0)
					break;
				cur_table_bits++;
				remainder <<= 1;
			}

			/* Create the entry that points from the main table to
			 * the subtable.  This entry contains the index of the
			 * start of the subtable and the number of bits with
			 * which the subtable is indexed (the log base 2 of the
			 * number of entries it contains).  */
			decode_table[cur_codeword_prefix] =
				HUFFDEC_SUBTABLE_POINTER |
				make_decode_table_entry(cur_table_start,
							cur_table_bits);

			/* Now that we're filling a subtable, we need to drop
			 * the first 'table_bits' bits of the codewords.  */
			num_dropped_bits = table_bits;
		}

		/* Create the decode table entry, which packs the decode result
		 * and the codeword length (minus 'table_bits' for subtables)
		 * together.  */
		u32 entry = make_decode_table_entry(decode_results[sym],
						codeword_len - num_dropped_bits);

		/* Fill in as many copies of the decode table entry as are
		 * needed.  The number of entries to fill is a power of 2 and
		 * depends on the codeword length; it could be as few as 1 or as
		 * large as half the size of the table.  Since the codewords are
		 * bit-reversed, the indices to fill are those with the codeword
		 * in its low bits; it's the high bits that vary.  */
		const unsigned end = cur_table_start + (1U << cur_table_bits);
		const unsigned increment = 1U << (codeword_len - num_dropped_bits);
                for(unsigned i = cur_table_start + (codeword_reversed >> num_dropped_bits) ;
                    i < end ;
                    i += increment)
                    decode_table[i] = entry;

		/* Advance to the next codeword by incrementing it.  But since
		 * our codewords are bit-reversed, we must manipulate the bits
		 * ourselves rather than simply adding 1.  */
		unsigned bit = 1U << (codeword_len - 1);
		while (codeword_reversed & bit)
			bit >>= 1;
		codeword_reversed &= bit - 1;
		codeword_reversed |= bit;

		/* Advance to the next symbol.  This will either increase the
		 * codeword length, or keep the same codeword length but
		 * increase the symbol value.  Note: since we are using
		 * bit-reversed codewords, we don't need to explicitly append
		 * zeroes to the codeword when the codeword length increases. */
		if (++sym_idx == num_syms)
			return true;
		len_counts[codeword_len]--;
		while (len_counts[codeword_len] == 0)
			codeword_len++;
	}
}




/* Build the decode table for the precode.  */
static bool
build_precode_decode_table(struct libdeflate_decompressor *d)
{
	/* When you change TABLEBITS, you must change ENOUGH, and vice versa! */
	STATIC_ASSERT(PRECODE_TABLEBITS == 7 && PRECODE_ENOUGH == 128);

	return table_builder::build_decode_table(d->u.l.precode_decode_table,
				  d->u.precode_lens,
				  DEFLATE_NUM_PRECODE_SYMS,
				  precode_decode_results,
				  PRECODE_TABLEBITS,
				  DEFLATE_MAX_PRE_CODEWORD_LEN,
				  d->working_space);
}

/* Build the decode table for the literal/length code.  */
static bool
build_litlen_decode_table(struct libdeflate_decompressor *d,
			  unsigned num_litlen_syms, unsigned num_offset_syms)
{
	/* When you change TABLEBITS, you must change ENOUGH, and vice versa! */
	STATIC_ASSERT(LITLEN_TABLEBITS == 10 && LITLEN_ENOUGH == 1334);

	return build_decode_table(d->u.litlen_decode_table,
				  d->u.l.lens,
				  num_litlen_syms,
				  litlen_decode_results,
				  LITLEN_TABLEBITS,
				  DEFLATE_MAX_LITLEN_CODEWORD_LEN,
				  d->working_space);
}

/* Build the decode table for the offset code.  */
static bool
build_offset_decode_table(struct libdeflate_decompressor *d,
			  unsigned num_litlen_syms, unsigned num_offset_syms)
{
	/* When you change TABLEBITS, you must change ENOUGH, and vice versa! */
	STATIC_ASSERT(OFFSET_TABLEBITS == 8 && OFFSET_ENOUGH == 402);

	return build_decode_table(d->offset_decode_table,
				  d->u.l.lens + num_litlen_syms,
				  num_offset_syms,
				  offset_decode_results,
				  OFFSET_TABLEBITS,
				  DEFLATE_MAX_OFFSET_CODEWORD_LEN,
				  d->working_space);
}

} /* namespace table_builder */

using table_builder::build_precode_decode_table;
using table_builder::build_offset_decode_table;
using table_builder::build_litlen_decode_table;
using table_builder::HUFFDEC_LENGTH_MASK;
using table_builder::HUFFDEC_RESULT_SHIFT;
using table_builder::HUFFDEC_SUBTABLE_POINTER;
using table_builder::HUFFDEC_LITERAL;
using table_builder::HUFFDEC_LENGTH_BASE_SHIFT;
using table_builder::HUFFDEC_EXTRA_LENGTH_BITS_MASK;
using table_builder::HUFFDEC_END_OF_BLOCK_LENGTH;
using table_builder::HUFFDEC_EXTRA_OFFSET_BITS_SHIFT;
using table_builder::HUFFDEC_OFFSET_BASE_MASK;

static forceinline machine_word_t
repeat_byte(byte b)
{
	machine_word_t v = static_cast<machine_word_t>(b);

	STATIC_ASSERT(WORDBITS == 32 || WORDBITS == 64);

	v |= v << 8;
	v |= v << 16;
	v |= v << ((WORDBITS == 64) ? 32 : 0);
	return v;
}

static forceinline void
copy_word_unaligned(const void *src, void *dst)
{
	store_word_unaligned(load_word_unaligned(src), dst);
}

/*****************************************************************************
 *                         Main decompression routine
 *****************************************************************************/




#include "decompress_impl.h"






LIBDEFLATEAPI struct libdeflate_decompressor *
libdeflate_alloc_decompressor(void)
{
	return new libdeflate_decompressor();
}

LIBDEFLATEAPI void
libdeflate_free_decompressor(struct libdeflate_decompressor *d)
{
	delete d;
}
