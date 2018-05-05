#ifndef SYNCHRONIZER_HPP
#define SYNCHRONIZER_HPP

#include <atomic>


/// Keep track of where to end the deconding in the current thread
struct alignas(64) synchronizer {

    // Post method
    void signal_first_decoded_sequence(size_t in_pos_blk, unsigned _first_seq_block_pos) {
        blk_start_in_pos.store(in_pos_blk, std::memory_order_release);
        first_seq_block_pos.store(_first_seq_block_pos, std::memory_order_release);
    }

    /** Observer method returning true if we started the block where
     * extracted it's first sequence
     */
    bool caught_up_block(size_t in_pos) {
        return in_pos >= blk_start_in_pos.load(std::memory_order_acquire);
    }

    /** Observer method returning true the block relative position block_pos is after
     * where the other thread extracted it's first sequence
     */
    bool caught_up_first_seq(unsigned block_pos) {
        return block_pos >= first_seq_block_pos.load(std::memory_order_acquire);
    }

protected:
    std::atomic<size_t> blk_start_in_pos = {~0UL};
    std::atomic<unsigned> first_seq_block_pos = {~0U};
};


#endif // SYNCHRONIZER_HPP
