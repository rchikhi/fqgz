#ifndef INSTR_DEFLATE_WINDOW_HPP
#define INSTR_DEFLATE_WINDOW_HPP

#include <vector>
#include <string>

#include "deflate_window.hpp"
#include "synchronizer.hpp"


static constexpr char ascii2Dna[256] =
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 2, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0,
     5, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 2, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 5, 0, 0, 0, 0, 0, 3,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

using uchar = unsigned char;

class InstrDeflateWindow : public StreamingDeflateWindow {
    using Base = StreamingDeflateWindow;

public:
    InstrDeflateWindow() :
        StreamingDeflateWindow(),
        has_dummy_32k(true), output_to_target(true),
        fully_reconstructed(false),
        nb_back_refs_in_block(0), len_back_refs_in_block(0),
        buffer_counts(new uint32_t[buffer_size]),
        backref_origins(new uint16_t[buffer_size])
    {
        clear();

        for (unsigned i = 0; i < context_size; i ++)
        {
            buffer[i] = byte('?');
            backref_origins[i] = context_size - i;
        }
    }

    void clear() {
        assert(has_dummy_32k);

        next = buffer+context_size;
        current_blk = next;

        for (unsigned i = 0; i < context_size; i ++)
        {
            buffer_counts[i] = 0;
        }
    }

    // record into a dedicated buffer that store counts of back references
    void record_match(wsize_t length, wsize_t offset) {
        size_t start = size() - offset;
        for (wsize_t i = 0; i < length; i++) {
            buffer_counts[size()+i] = ++buffer_counts[start+i];
            backref_origins[size()+i] = backref_origins[start+i];
        }

        nb_back_refs_in_block++;
        len_back_refs_in_block += length;
    }

    bool check_match(wsize_t length, wsize_t offset) {
        //if (debug) fprintf(stderr,"want to copy match of length %d, offset %d (window size %d)\n",(int)length,(int)offset, size());
        /* The match source must not begin before the beginning of the
         * output buffer.  */
        if (offset > size() )
        {
            PRINT_DEBUG("fail, copy_match, offset %d (window size %d)\n",(int)offset, size());
            return false;
        }

        if (offset ==0)
        {
            PRINT_DEBUG("fail, copy_match, offset 0\n");
            return false;
        }
        return true;
    }

    bool push(byte c) {
        if( c > byte('c') || c < byte('\t')) {
            PRINT_DEBUG("fail, unprintable literal unexpected in fastq\n");
            return false;
        }

        buffer_counts[size()] = 0;
        backref_origins[size()] = 0;
        Base::push(c);

        return true;
    }

    void copy_match(wsize_t length, wsize_t offset) {
        record_match(length, offset);
        Base::copy_match(length, offset);
    }

    void copy(InputStream & in, wsize_t length) {
        size_t start = size();
        for(size_t i=start ; i < start + length ; i++) {
            buffer_counts[i]=0;
            backref_origins[i]=0;
        }
        Base::copy(in, length);
    }

    bool check_ascii() {
        wsize_t start = has_dummy_32k ? context_size : 0;
        wsize_t dec_size = size() - start;
        if(dec_size < 1024) { // Required decoded size to properly assess block synchronization
            return false;
        }

        wsize_t ascii_found = 0;
        for(wsize_t i = start ; i < size() ; i++) {
            byte c = buffer[i];
            assert(c > byte('~'));
            if(c != byte('?')) {
                ascii_found++;
            }
        }

        return ascii_found >= dec_size/4;
    }

    // make sure the buffer contains at least something that looks like fastq
    // funny story: sometimes a bad buffer may contain many repetitions of the same letter
    // anyhow, this function could be vastly improved by penalizing non-ACTG chars
    bool check_buffer_fastq(bool previously_aligned, wsize_t review_len=context_size)
    {
        if (size() < (has_dummy_32k ? context_size : 0))
            return false; // block too small, nothing to do


        PRINT_DEBUG("potential good block, beginning fastq check, bounds %ld %ld\n",next-context_size - buffer, size());
        // check the first 5K, mid 5K, last 5K
        wsize_t check_size = 5000;
        wsize_t pos[3] = { size() - review_len, size() - review_len/2, size() - check_size };
        for (auto start: pos)
        {
            unsigned dna_letter_count = 0;
            size_t letter_histogram[5] = {0,0,0,0,0}; // A T G C N
            for (wsize_t  i = start ;i < start + check_size; i ++)
            {
                auto c = (unsigned char)(buffer[i]);
                if(c > '~') {
                    return false;
                }
                uint8_t code = ascii2Dna[c];
                if(code > 0) {
                    letter_histogram[code-1]++;
                    dna_letter_count++;
                }
            }

            bool ok = dna_letter_count > check_size/10;
            for(wsize_t i =0 ; ok & (i < 4) ; i ++) { // Check A, T, G, & C counts (not N)
                ok &= letter_histogram[i] > 20;
            }

            if (!ok) /* some 10K block will have 20 A's,C's,T's,G's, but just not enough */
            {
                if (previously_aligned)
                {
                    fprintf(stderr,"bad block after we thought we had a good block. let's review it:\n");
                    fwrite(next - review_len, 1, review_len, stderr);
                }
                return false;
            }
        }
        return true;
    }

    // decide if the buffer contains all the fastq sequences with no ambiguities in them
    void check_fully_reconstructed_sequences(synchronizer* stop, bool last_block, wsize_t review_size=context_size)
    {
        if (size() < review_size)
        {
            fprintf(stderr,"not enough context to check context. should that happen?\n"); // i don't think so but worth checking
            exit(1);
            return;
        }

        // when this function is called, we're at the start of a block, so here's the context start
        long int start_pos =  size() - review_size;

        // get_sequences_between_separators(); inlined
        std::vector<std::string> putative_sequences;
        std::string current_sequence = ""; current_sequence.reserve(256);
        wsize_t current_sequence_pos = start_pos;
        for (long int i = start_pos; i < next-buffer; i ++)
        {
            bool after_current_block = i >= current_blk - buffer; // FIXME: sync parser such that this is always true
            if(stop != nullptr && last_block && after_current_block && stop->caught_up_first_seq(i - (current_blk - buffer)))
                 break; // We reached the first sequence decoded by the next thread

            uchar c = uchar(buffer[i]);
            if (ascii2Dna[c] > 0)
                current_sequence += c;
            else
            {
                if ((c == '\r' || c == '\n' || c == '?')  //(buffer_counts[i] > 1000))  // actually not a good heursitic
                        && (current_sequence.size() > 30)) // heuristics here, assume reads at > 30 bp
                {
                    // Record the position of the first decoded sequence relative to the block start
                    // Note: this won't be used untill fully_reconstructed is turned on, so no risks of putting garbage here
                    if(after_current_block) // FIXME: sync parser such that is always true
                        first_seq_block_pos = current_sequence_pos - (current_blk - buffer);

                    last_processed = buffer + i;

                    putative_sequences.push_back(current_sequence);
                    // note this code may capture stretches of quality values too
                }
                current_sequence = "";
                current_sequence_pos = i+1;
            }
        }
        if (current_sequence.size() > 30)
            putative_sequences.push_back(current_sequence);

        // get_sequences_length_histogram(); inlined
        // first get maximum histogram length
        int max_histogram = 0;
        for (auto seq: putative_sequences)
            max_histogram = std::max(max_histogram,(int)(seq.size()));

        max_histogram++; // important, because we'll iterate and allocate array with numbers up to this bound

        if (max_histogram > 10000)
            fprintf(stderr,"warning: maximum putative read length %d, not supposed to happen if we have short reads", max_histogram);

        // init histogram
        std::vector<int> histogram(max_histogram);
        for (int i = 0; i < max_histogram; i++)
            histogram[i] = 0;

        // populate it
        for (auto seq: putative_sequences)
            histogram[seq.size()] += 1;

        // compute sum
        int nb_reads = 0;
        for (int i = 0; i < max_histogram; i++)
            nb_reads += histogram[i];

        // heuristic: check that all sequences are concentrated in a single value (assumes that all reads have same read length)
        // allows for beginning and end of block reads to be truncated, so - 1
        bool res = false;
        for (int len = 1; len < max_histogram; len++)
        {
            if (histogram[len] >= nb_reads - 2)
            {
                // heuristic: compute a lower bound on  number of reads in that context
                // assumes that fastq file size is at most 4x the size of sequences
                int min_nb_reads = context_size/(4*len);
                if (nb_reads < min_nb_reads)
                    break;

                res = true;
            }
        }

        //pretty_print();
        PRINT_DEBUG("check_fully_reconstructed status: total buffer size %d, ", (int)(next-buffer));
        if (res) {
            PRINT_DEBUG("fully reconstructed %d reads of length %d\n", nb_reads, readlen); // continuation of heuristic
        } else {
            PRINT_DEBUG("incomplete, %d reads\n ", nb_reads);
        }

        if (res == false)
        {
            for (int i = 0; i < max_histogram; i++)
            {
                if (histogram[i] > 0)
                    PRINT_DEBUG("histogram[%d]=%d ",i,histogram[i]);
            }
            PRINT_DEBUG("\n");
        }
        fully_reconstructed |= res;
        DEBUG_FIRST_BLOCK(exit(1);)
    }

    // debug only
    wsize_t dump(byte* const dst) {
        memcpy(dst, buffer, size());
        return size();
    }

    void pretty_print()
    {
       const char*  KNRM = "\x1B[0m";
      const char*  KRED = "\x1B[31m";
      const char*  KGRN = "\x1B[32m";
      const char*  KYEL = "\x1B[33m";/*
      const char*  KBLU = "\x1B[34m";
      const char*  KMAG = "\x1B[35m";
      const char*  KCYN = "\x1B[36m";
      const char*  KWHT = "\x1B[37m";*/
        fprintf(stderr, "%s about to print a window %s\n", KRED, KNRM);
        wsize_t length = size();
        for (wsize_t i = 0; i < length; i++)
        {
            char const *color;
            if (buffer_counts[i] < 10)
                color = KNRM;
            else
            {
                if (buffer_counts[i] < 100)
                   color = KGRN;
                else
                {
                    if (buffer_counts[i] < 1000)
                       color = KYEL;
                    else
                       color = KRED;
                }
            }
            if (buffer[i] == byte('\n'))
                fprintf(stderr,"%s\\n%c%s",color,char(buffer[i]),KNRM);
            else {
                if(backref_origins[i] > 0) { // Live reference to the unknown initial context window
                    assert(buffer[i]== byte('?'));

                    // Compute the monotone span of backreferences to the unknown primary window
                    uint16_t start = backref_origins[i]; // First offset
                    uint16_t end = start;
                    do { // Lookahead
                        i++;
                        end--;
                    } while(i < length && backref_origins[i] == end);
                    i--; // Backtrack to last correct position

                    if(start-end == 1) { // No monotone span found (singleton)
                        // It might be a repeated singleton backref (common)
                        unsigned count = 0;
                        do { // Lookahead
                            count++;
                            i++;
                        } while(i < length && backref_origins[i] == start);
                        i--; // Backtrack to last correct position

                        if(count > 1) {
                            fprintf(stderr,"%s[%dx%d]%s",color,start, count,KNRM);
                        } else {
                            fprintf(stderr,"%s[%d]%s",color,start ,KNRM);
                        }
                    } else {
                        fprintf(stderr,"%s[%d,%d]%s",color,start, start-end ,KNRM);
                    }
                } else {
                    fprintf(stderr,"%s%c%s",color,char(buffer[i]),KNRM);
                }
            }
        }
    }

    /* called when the window is full.
     * note: not necessarily at the end of a block */
    void flush(size_t keep_size=context_size) {
        size_t moved_by = Base::flush(keep_size);
        assert(!has_dummy_32k || moved_by > 1UL <<15);

        // update counts
        memcpy(buffer_counts, buffer_counts + size() - keep_size, keep_size*sizeof(uint32_t));
        memcpy(backref_origins, backref_origins + size() - keep_size, keep_size*sizeof(uint16_t));

        has_dummy_32k = false;
    }

    void notify_end_block(bool is_final_block, InputStream& in_stream){
        PRINT_DEBUG("block size was %ld bits left %ld overrun_count %lu nb back refs %u tot/average len %u/%.1f\n",
                    next-current_blk,
                    in_stream.bitsleft,
                    in_stream.overrun_count,
                    nb_back_refs_in_block,
                    len_back_refs_in_block,
                    1.0*len_back_refs_in_block/nb_back_refs_in_block);

        current_blk = next;
        nb_back_refs_in_block = 0;
        len_back_refs_in_block = 0;
    }

    byte* current_blk;
    wsize_t first_seq_block_pos = ~0U; /// Position of the first sequence relative to the current block start

    bool has_dummy_32k; // flag whether the window contains the initial dummy 32k context
    bool output_to_target; // flag whether, during a flush, window content should be copied to target or discarded (when scanning the first 20 blocks)
    bool fully_reconstructed; // flag to say whether context is fully reconstructed (heuristic)

    // some back-references statistics
    unsigned nb_back_refs_in_block;
    unsigned len_back_refs_in_block;

    uint32_t /* const (FIXME, Rayan: same as InputStream*/ *buffer_counts; /// Allocated counts for keeping track of how many back references in the buffer
    // Offsets in the primary unknown context window
    // initially backref_origins[context_size - 1]=1, backref_origins[context_size - 2]=2, etc
    uint16_t* backref_origins;
};

#endif // INSTR_DEFLATE_WINDOW_HPP
