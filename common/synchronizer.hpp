#ifndef SYNCHRONIZER_HPP
#define SYNCHRONIZER_HPP

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <memory>

/// Keep track of where to end the deconding in the current thread
struct alignas(64) synchronizer {
    using context_ptr = std::unique_ptr<char[]>;

    // Post method
    void signal_first_decoded_sequence(size_t in_pos_blk, unsigned _first_seq_block_pos) {
        blk_start_in_pos.store(in_pos_blk, std::memory_order_release);
        first_seq_block_pos.store(_first_seq_block_pos, std::memory_order_release);
    }

    /** Observer method returning true if we started the block where
     * the other thread extracted it's first sequence
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

    // Post context and wait for it to be consumed
    void post_context(context_ptr&& ctx) {
        auto lock = std::unique_lock<std::mutex>(mut);
        this->context = std::move(ctx);
        cond.notify_all();
    }

   context_ptr get_context() {
        auto lock = std::unique_lock<std::mutex>(mut);
        while(!context) cond.wait(lock);
        return std::move(context);
    }

    /// Signal the thread doing the next chunk that we have emitted our output.
    void signal_output() {
        auto lock = std::unique_lock<std::mutex>(mut);
        prev_emitted_output = true;
        cond.notify_all();
    }

    /// Waits for the thread doing the previous block to emmit it's output
    void wait_output() {
        auto lock = std::unique_lock<std::mutex>(mut);
        while(!prev_emitted_output) cond.wait(lock);
    }

protected:
    std::mutex mut {};
    std::condition_variable cond {};
    std::atomic<size_t> blk_start_in_pos = {~0UL};
    std::atomic<unsigned> first_seq_block_pos = {~0U};

    context_ptr context = nullptr;
    bool prev_emitted_output = false;
};


#endif // SYNCHRONIZER_HPP
