#ifndef INPUT_STREAM_HPP
#define INPUT_STREAM_HPP

#include <cstring> // memcpy

#include "common_defs.h"
#include "assert.hpp"
#include "unaligned.h"

/**
 * @brief Model an compressed gzip input stream
 * It can be read by dequeing n<32 bits at a time or as byte aligned u16 words

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
 */
class InputStream {
public: //protected: //FIXME
    /*
     * The type for the bitbuffer variable ('bitbuf' described above).  For best
     * performance, this should have size equal to a machine word.
     *
     * 64-bit platforms have a significant advantage: they get a bigger bitbuffer
     * which they have to fill less often.
     */
    using bitbuf_t = machine_word_t;
    using bitbuf_size_t = uint_fast32_t;

    /** State */
    const byte *const begin;
    const byte *restrict in_next; /// Read pointer
    const byte *restrict const in_end; /// Adress of the byte after input
    bitbuf_t bitbuf = bitbuf_t(0); /// Bit buffer
    bitbuf_size_t bitsleft = 0; /// Number of valid bits in the bit buffer
    bitbuf_size_t overrun_count = 0;
    bool reached_final_block = false;

    /**
     * Number of bits the bitbuffer variable can hold.
     */
    static constexpr bitbuf_size_t bitbuf_length = 8 * sizeof(bitbuf_t);

    /**
     * The maximum number of bits that can be requested to be in the bitbuffer
     * variable.  This is the maximum value of 'n' that can be passed
     * ensure_bits(n).
     *
     * This not equal to BITBUF_NBITS because we never read less than one byte at a
     * time.  If the bitbuffer variable contains more than (BITBUF_NBITS - 8) bits,
     * then we can't read another byte without first consuming some bits.  So the
     * maximum count we can ensure is (BITBUF_NBITS - 7).
     */
    static constexpr bitbuf_size_t bitbuf_max_ensure = bitbuf_length - 7;

    /**
     * Does the bitbuffer variable currently contain at least 'n' bits?
     */
    inline bool have_bits(size_t n) const
    { return bitsleft >= n; }

    /**
     * Fill the bitbuffer variable by reading the next word from the input buffer.
     * This can be significantly faster than fill_bits_bytewise().  However, for
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
                       bitbuf |= bitbuf_t(*in_next++) << bitsleft;
               else
                       overrun_count++;
               bitsleft += 8;
        } while (bitsleft <= bitbuf_length - 8);
    }

public:
    InputStream(const byte* in, size_t len) :
        begin(in), in_next(in), in_end(in + len)
    {}

    InputStream(const InputStream&) = default;

    /// states asignments for the same stream (for backtracking)
    InputStream& operator=(const InputStream& from) {
        assert(begin == from.begin && in_end == from.in_end);

        in_next = from.in_next;
        bitbuf = from.bitbuf;
        bitsleft = from.bitsleft;
        overrun_count = from.overrun_count;
        reached_final_block = from.reached_final_block;
        return *this;
    }

    InputStream(InputStream&&) = default;
    InputStream& operator=(InputStream&&) = default;

    size_t size() const {
        assert(in_end >= begin);
        return in_end - begin;
    }

    /** Remaining available bytes
     * @note align_input() should be called first in order to get accurate readings (or use available_bits() / 8)
     */
    size_t available() const {
        assert(in_end >= in_next);
        return in_end - in_next;
    }

    /// Remaining available bits
    size_t available_bits() const {
        return 8*available() + bitsleft;
    }

    /** Position in the stream in bytes
     * @note align_input() should be called first in order to get accurate readings (or use position_bits() / 8)
     */
    size_t position() const {
        assert(in_next >= begin);
        return in_next - begin;
    }

    size_t position_bits() const {
        return 8*position() - bitsleft;
    }

    /// Seek forward
    void skip(size_t offset) {
        align_input();
        assert(in_next + offset < in_end);
        in_next += offset;
    }

    /**
     * Load more bits from the input buffer until the specified number of bits is
     * present in the bitbuffer variable.  'n' cannot be too large; see MAX_ENSURE
     * and CAN_ENSURE().
     */
    template <bitbuf_size_t n>
    bool ensure_bits() {
        static_assert(n <= bitbuf_max_ensure, "Bit buffer is too small");
        if (!have_bits(n)) {
                if(unlikely(available() < 1))
                    return false; // This is not acceptable overrun
                if (likely(available() >= sizeof(bitbuf_t)))
                    fill_bits_wordwise();
                else
                    fill_bits_bytewise();
        }
        return true;
    }

    /**
     * Return the next 'n' bits from the bitbuffer variable without removing them.
     */
    u32 bits(bitbuf_size_t n) const {
        assert(bitsleft >= n);
        return  u32(bitbuf & ((u32(1) << n) - 1));
    }

    /**
     * Remove the next 'n' bits from the bitbuffer variable.
     */
    inline void remove_bits(bitbuf_size_t n) {
        assert(bitsleft >= n);
        bitbuf >>= n;
        bitsleft -= n;
    }

    /**
     * Remove and return the next 'n' bits from the bitbuffer variable.
     */
    inline u32 pop_bits(bitbuf_size_t n) {
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
        assert(overrun_count <= (bitsleft >> 3));
        in_next -= (bitsleft >> 3) - overrun_count;
        // was:
        //in_next -= (bitsleft >> 3) - std::min(overrun_count, bitsleft >> 3);
        bitbuf = 0;
        bitsleft = 0;
    }

    /**
     * Read a 16-bit value from the input.  This must have been preceded by a call
     * to ALIGN_INPUT(), and the caller must have already checked for overrun.
     */
    inline u16 pop_u16() {
        assert(available() >= 2);
        u16 tmp = get_unaligned_le16(in_next);
        in_next += 2;
        return tmp;
    }


    /**
      * Copy n bytes to the ouput buffer. The input buffer must be aligned with a
      * call to align_input()
      */
    inline void copy(byte* restrict out, size_t n) {
        assert(available() >= n);
        memcpy(out, in_next, n);
        in_next += n;
    }

    /**
      * Checks that the lenght next bytes are ascii
      * (for checked copy()ies of uncompressed blocks)
      */
    inline bool check_ascii(size_t n) {
        if(unlikely(n > available()))
            return false;

        for(size_t i=0 ; i < n ; i++) {
            byte c = in_next[i];
            if(c > byte('c') || c < byte('\t'))
                return false;
        }
        return true;
    }

};

#endif // INPUT_STREAM_HPP
