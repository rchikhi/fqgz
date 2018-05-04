#ifndef DEFLATE_WINDOW_HPP
#define DEFLATE_WINDOW_HPP

#include <cstdint>
#include <algorithm>

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

/**
 * @brief A window of the size of one decoded shard (some deflate blocks) plus it's 32K context
 */
template<unsigned _buffer_bits = 21, unsigned _context_bits = 15>
struct DeflateWindow
{
    // TODO: should we set these at runtime ? context_bits=15 should be fine for all compression levels
    // buffer_size could be adjusted on L3 cache size.
    // But: there is a runtime cost as we loose some optimizations
    static constexpr unsigned buffer_bits = _buffer_bits;
    static constexpr unsigned context_bits = _context_bits;
    static_assert(context_bits < buffer_bits, "Buffer too small for context");

    using wsize_t = uint_fast32_t; /// Type for positive offset in buffer
    using wssize_t = int_fast32_t; /// Type for signed offsets in buffer
    static_assert(std::numeric_limits<wsize_t>::max() > (1ULL << buffer_bits), "Buffer too large for wsize_t");
    static_assert(std::numeric_limits<wssize_t>::max() > (1ULL << buffer_bits), "Buffer too large for wssize_t");

    static constexpr wsize_t context_size = wsize_t(1) << context_bits;
    static constexpr wsize_t buffer_size = wsize_t(1) << buffer_bits;

    DeflateWindow()
      : buffer(new byte[buffer_size])
      , buffer_end(buffer + buffer_size)
      , next(buffer)
    {}

    ~DeflateWindow()
    {
        if (next != nullptr)
            delete[] buffer;
    }

    DeflateWindow(DeflateWindow&& from)
      : buffer(from.buffer)
      , buffer_end(buffer + buffer_size)
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
        *next++ = c;
        return true;
    }

    /* return true if it's a reasonable offset, otherwise false */
    bool copy_match(wsize_t length, wsize_t offset)
    {
        /* The match source must not begin before the beginning of the
         * output buffer.  */
        assert(offset <= size());
        assert(available() >= length);

        if (length <= (3 * WORDBYTES) && offset >= WORDBYTES && length + (3 * WORDBYTES) <= available()) {
            /* Fast case: short length, no overlaps if we copy one
             * word at a time, and we aren't getting too close to
             * the end of the output array.  */
            copy_word_unaligned(next - offset + (0 * WORDBYTES), next + (0 * WORDBYTES));
            copy_word_unaligned(next - offset + (1 * WORDBYTES), next + (1 * WORDBYTES));
            copy_word_unaligned(next - offset + (2 * WORDBYTES), next + (2 * WORDBYTES));
        } else {
            const byte* src = next - offset;
            byte* dst = next;
            const byte* const dst_end = dst + length;

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
        memcpy(buffer, next - keep_size, keep_size);
        size_t moved_by = size() - keep_size;

        // update next pointer
        next = buffer + keep_size;

        return moved_by;
    }

    bool notify_end_block(InputStream& in_stream) const { return true; }

  protected:
    void clone_context(const DeflateWindow& from)
    {
        assert(next == buffer); // Empty state: call clear() before.
        assert(from.size() >= context_size);
        memcpy(buffer, from.next - context_size, context_size);
        next += context_size;
    }

    static forceinline machine_word_t repeat_byte(byte b)
    {
        machine_word_t v = static_cast<machine_word_t>(b);

        STATIC_ASSERT(WORDBITS == 32 || WORDBITS == 64);

        v |= v << 8;
        v |= v << 16;
        v |= v << ((WORDBITS == 64) ? 32 : 0);
        return v;
    }

    static forceinline void copy_word_unaligned(const void* src, void* dst) { store_word_unaligned(load_word_unaligned(src), dst); }

    byte* const buffer;           /// Allocated output buffer
    const byte* const buffer_end; /// Past the end pointer
    byte* next;                   /// Next byte to be written
};

class FlushableDeflateWindow : public DeflateWindow<>
{
    using Base = DeflateWindow;

  public:
    FlushableDeflateWindow(byte* _target, byte* _target_end)
      : DeflateWindow()
      , target(_target)
      , target_start(_target)
      , target_end(_target_end)
    {}

    wsize_t flush(wsize_t start = 0, wsize_t window_size = context_size)
    {
        assert(size() >= window_size);
        size_t evict_size = size() - window_size - start;
        assert(buffer + start + evict_size == next - window_size);
        assert(target + evict_size < target_end);

        memcpy(target, buffer + start, evict_size);
        target += evict_size;

        // Then move the context to the start
        return DeflateWindow::flush(window_size);
    }

    wsize_t get_evicted_length()
    {
        assert(target >= target_start);
        return wsize_t(target - target_start);
    }

  protected:
    byte* target;
    const byte* target_start;
    const byte* target_end;
};

template<typename _Base = DeflateWindow<>, byte _unknown_fill = byte('?')>
struct SyncingDeflateWindow : public _Base
{
    using Base = _Base;
    using Base::buffer;
    using Base::context_size;
    using Base::size;
    using typename Base::wsize_t;

    static constexpr byte unknown_fill = _unknown_fill;

    SyncingDeflateWindow()
      : Base()
    {
        memset(buffer, int(unknown_fill), context_size);
        clear();
    }

    void clear(wsize_t begin = 0)
    {
        //        assert(std::all_of(buffer, buffer + context_size,
        //                           [](const byte& b){ return b == unknown_fill; }));
        Base::clear(begin + context_size);
    }

    bool push(byte c)
    {
        if (c > byte('~') || c < byte('\t')) {
            PRINT_DEBUG("fail, unprintable literal unexpected in fastq\n");
            return false;
        }

        return Base::push(c);
    }

    wsize_t flush(size_t = 0)
    {
        assert(false); // FIXME: Could this still happen ? Comment this if it's a acceptable sync faillure
        return 0;
    }

    bool copy(InputStream& in, wsize_t length)
    {
        if (in.check_ascii(length)) {
            return Base::copy(in, length);
        }
        PRINT_DEBUG("fail, unprintable uncompressed segment unexpected in fastq\n");
        return false;
    }

    bool copy_match(wsize_t length, wsize_t offset)
    {
        /* The match source must not begin before the beginning of the
         * output buffer.  */
        if (offset > context_size || offset > size()) {
            PRINT_DEBUG("fail, copy_match, offset %d (window size %d)\n", (int)offset, size());
            return false;
        }

        assert(offset <= size()); // since size() >= context_size
        assert(offset != 0);      // Could not happen with the way offset are encoded

        return Base::copy_match(length, offset);
    }
};

/// Keeps a pointer last_processed limiting the flush
/// Also keeps track of the current_blk, allowing multithread synchronization
struct StreamingDeflateWindow : public DeflateWindow<>
{
    StreamingDeflateWindow()
      : DeflateWindow()
    {
        clear();
    }

    void clear(size_t begin = 0)
    {
        DeflateWindow::clear(begin);
        last_processed = next;
        current_blk = next;
    }

    size_t flush(size_t& keep_size)
    {
        assert(next >= last_processed);
        keep_size = std::max(keep_size, size_t(next - last_processed));
        size_t moved_by = DeflateWindow::flush(keep_size);

        current_blk -= moved_by;
        last_processed -= moved_by;
        assert(last_processed >= buffer);

        return moved_by;
    }

    void notify_end_block() { current_blk = next; }

    ssize_t last_processed_pos_in_block() { return last_processed - current_blk; }

  protected:
    byte* last_processed;
    byte* current_blk;
};

#endif // DEFLATE_WINDOW_HPP
