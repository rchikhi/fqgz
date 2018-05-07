#ifndef DEFLATE_WINDOW_HPP
#define DEFLATE_WINDOW_HPP

#include <cstdint>
#include <algorithm>
#include <limits>

#include "common_defs.h"
#include "assert.hpp"
#include "unaligned.h"
#include "input_stream.hpp"

#define PRINT_DEBUG(...)                                                                                                                                       \
    {}
//#define PRINT_DEBUG(...) {fprintf(stderr, __VA_ARGS__);}
#define DEBUG_FIRST_BLOCK(x)                                                                                                                                   \
    {}
//#define DEBUG_FIRST_BLOCK(x) {x}

template<typename to_t = size_t, typename from_t>
static inline to_t
repeat_bits(from_t b)
{
    constexpr size_t n = sizeof(to_t) / sizeof(from_t);                              // How many from_t fit in to_t
    static_assert(sizeof(to_t) % sizeof(from_t) == 0 && n > 0 && (n & (n - 1)) == 0, // Ratio is an integer power of two greater than 1
                  "A power of two from_t value must fit in on to_t value");

    to_t v = to_t(b);
    for (unsigned i = sizeof(from_t); i < sizeof(to_t); i *= 2)
        v |= v << (i * 8);

    return v;
}

/**
 * @brief A window of the size of one decoded shard (some deflate blocks) plus it's 32K context
 */
template<typename _char_t = char, unsigned _buffer_bits = 21, unsigned _context_bits = 15>
struct DeflateWindow
{
    // TODO: should we set these at runtime ? context_bits=15 should be fine for all compression levels
    // buffer_size could be adjusted on L3 cache size.
    // But: there is a runtime cost as we loose some optimizations
    static constexpr unsigned buffer_bits = _buffer_bits;
    static constexpr unsigned context_bits = _context_bits;
    static_assert(context_bits < buffer_bits, "Buffer too small for context");

    using char_t = _char_t;
    static constexpr char_t max_value = 255, min_value = 0;

    using wsize_t = uint_fast32_t; /// Type for positive offset in buffer
    using wssize_t = int_fast32_t; /// Type for signed offsets in buffer
    static_assert(std::numeric_limits<wsize_t>::max() > (1ULL << buffer_bits), "Buffer too large for wsize_t");
    static_assert(std::numeric_limits<wssize_t>::max() > (1ULL << buffer_bits), "Buffer too large for wssize_t");

    static constexpr wsize_t context_size = wsize_t(1) << context_bits;
    static constexpr wsize_t buffer_size = wsize_t(1) << buffer_bits;

    DeflateWindow()
      : buffer(new char_t[buffer_size])
      , buffer_end(buffer + buffer_size)
      , next(buffer)
    {}

    ~DeflateWindow()
    {
        if (next != nullptr) // Not in a moved-from state
            delete[] buffer;
    }

    DeflateWindow(DeflateWindow&& from)
      : buffer(from.buffer)
      , buffer_end(from.buffer + buffer_size)
      , next(from.next)
    {
        from.next = nullptr;
    }

    /// Clone the context window
    DeflateWindow(const DeflateWindow& from)
      : DeflateWindow()
    {
        clone_context(from);
    }

    /// Clone the context window
    DeflateWindow& operator=(const DeflateWindow& from)
    {
        clear();
        clone_context(from);
    }

    void clear(size_t begin = 0) { next = buffer + begin; }

    wsize_t size() const
    {
        assert(next >= buffer);
        return next - buffer;
    }

    wsize_t available() const
    {
        assert(buffer_end >= next);
        return buffer_end - next;
    }

    bool push(byte c)
    {
        assert(available() >= 1); // Enforced by do_block
        *next++ = char_t(c);
        return true;
    }

    /* return true if it's a reasonable offset, otherwise false */
    bool copy_match(wsize_t length, wsize_t offset)
    {
        /* The match source must not begin before the beginning of the
         * output buffer.  */
        if (offset > context_size) {
            PRINT_DEBUG("fail, copy_match, offset %d (window size %d)\n", (int)offset, size());
            return false;
        }

        // Could not happen with the way offset and length are encoded
        assert(length >= 3);
        assert(offset != 0);
        // do_block must guard against overflow:
        assert(available() >= length);

        static constexpr wsize_t word_chars = sizeof(machine_word_t) / sizeof(char_t);

        if (length <= (3 * word_chars) && offset >= word_chars && length + (3 * word_chars) <= available()) {
            /* Fast case: short length, no overlaps if we copy one
             * word at a time, and we aren't getting too close to
             * the end of the output array.  */
            copy_word_unaligned(next - offset + (0 * word_chars), next + (0 * word_chars));
            copy_word_unaligned(next - offset + (1 * word_chars), next + (1 * word_chars));
            copy_word_unaligned(next - offset + (2 * word_chars), next + (2 * word_chars));
        } else {
            const char_t* src = next - offset;
            char_t* dst = next;
            const char_t* const dst_end = dst + length;

            if (likely(size_t(buffer_end - dst_end) >= word_chars - 1)) {
                if (offset >= word_chars) {
                    copy_word_unaligned(src, dst);
                    src += word_chars;
                    dst += word_chars;
                    if (dst < dst_end) {
                        do {
                            copy_word_unaligned(src, dst);
                            src += word_chars;
                            dst += word_chars;
                        } while (dst < dst_end);
                    }
                } else if (offset == 1) {
                    machine_word_t v = repeat_bits(*(dst - 1));
                    do {
                        store_word_unaligned(v, dst);
                        src += word_chars;
                        dst += word_chars;
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

        DEBUG_FIRST_BLOCK(fprintf(stderr, "match of length %d offset %d: ", length, offset);)
        DEBUG_FIRST_BLOCK(for (wsize_t int i = 0; i < length; i++) fprintf(stderr, "%c", buffer[next + i - buffer]); fprintf(stderr, "\n");)

        next += length;

        return true;
    }

    bool copy(InputStream& in, wsize_t length)
    {
        assert(available() >= length);
        in.copy(next, length);
        next += length;
        return true;
    }

    /// Move the 32K context to the start of the buffer
    size_t flush(size_t keep_size = context_size)
    {
        assert(size() > keep_size);
        assert(buffer + keep_size <= next - keep_size); // src and dst aren't overlapping
        memcpy(buffer, next - keep_size, keep_size * sizeof(char_t));
        size_t moved_by = size() - keep_size;

        // update next pointer
        next = buffer + keep_size;

        return moved_by;
    }

    bool notify_end_block(InputStream& in_stream) const { return true; }

    char_t* const buffer;           /// Allocated output buffer
    const char_t* const buffer_end; /// Past the end pointer
    char_t* next;                   /// Next char_t to be written

  protected:
    void clone_context(const DeflateWindow& from)
    {
        assert(next == buffer); // Empty state: call clear() before.
        assert(from.size() >= context_size);
        memcpy(buffer, from.next - context_size, context_size * sizeof(char_t));
        next += context_size;
    }

    static forceinline void copy_word_unaligned(const void* src, void* dst) { store_word_unaligned(load_word_unaligned(src), dst); }
};

template<typename _Base = DeflateWindow<>>
class AsciiOnly : public _Base
{
    using Base = _Base;

  public:
    using Base::Base;
    using typename Base::char_t;
    using typename Base::wsize_t;

    static constexpr char_t max_value = char_t('~'), min_value = char_t('\t');

    bool push(byte c)
    {
        if (char_t(c) > max_value || char_t(c) < min_value) {
            PRINT_DEBUG("fail, unprintable literal unexpected in fastq\n");
            return false;
        }

        return Base::push(c);
    }

    bool copy(InputStream& in, wsize_t length)
    {
        if (in.check_ascii(length)) {
            return Base::copy(in, length);
        }
        PRINT_DEBUG("fail, unprintable uncompressed block unexpected in fastq\n");
        return false;
    }
};

template<typename _Base = DeflateWindow<>>
class DummyContext : public _Base
{
    using Base = _Base;

  public:
    using Base::Base;
    using typename Base::wsize_t;

    void clear(wsize_t begin = 0) { Base::clear(begin + this->context_size); }

    wsize_t flush(wsize_t = 0) { return 0; }
};

template<typename _Base = DeflateWindow<>, typename _Base::char_t unknown_fill = typename _Base::char_t('?')>
class FillDummyContext : public DummyContext<_Base>
{
    using Base = DummyContext<_Base>;

  public:
    using Base::buffer;
    using typename Base::char_t;

    FillDummyContext()
      : Base()
    {
        memset(buffer, int(unknown_fill), this->context_size);
        Base::clear();
    }
};

template<typename _Base = AsciiOnly<DeflateWindow<uint16_t>>>
class SymbolicDummyContext : public DummyContext<_Base>
{
    using Base = DummyContext<_Base>;

  public:
    using typename Base::char_t;
    using typename Base::wsize_t;

    static_assert(size_t(Base::max_value) + size_t(Base::context_size) + 1 <= size_t(std::numeric_limits<char_t>::max()),
                  "Not enough value space in char_t to encode context symbols");

    SymbolicDummyContext()
      : Base()
    {
        for (wsize_t i = 0; i < this->context_size; i++)
            this->buffer[i] = char_t(i) + this->max_value + 1;

        Base::clear();

        assert(*(this->next - this->context_size) - (this->max_value + 1) == 0);
        assert(*(this->next - 1) - (this->max_value + 1) == this->context_size - 1);
    }
};

/// Keeps a pointer last_processed limiting the flush
/// Also keeps track of the current_blk, allowing multithread synchronization
template<typename _Base = DeflateWindow<>>
class StreamingDeflateWindow : public _Base
{
    using Base = _Base;

  public:
    using typename Base::char_t;

    StreamingDeflateWindow()
      : Base()
    {
        Base::clear();
    }

    void clear(size_t begin = 0)
    {
        Base::clear(begin);
        last_processed = this->next;
        current_blk = this->next;
    }

    size_t flush(size_t& keep_size)
    {
        assert(this->next >= last_processed);
        keep_size = std::max(keep_size, size_t(this->next - last_processed));
        size_t moved_by = Base::flush(keep_size);

        current_blk -= moved_by;
        last_processed -= moved_by;
        assert(last_processed >= this->buffer);

        return moved_by;
    }

    void notify_end_block() { current_blk = this->next; }

    ssize_t last_processed_pos_in_block() { return last_processed - current_blk; }

  protected:
    char_t* last_processed;
    char_t* current_blk;
};

#endif // DEFLATE_WINDOW_HPP
