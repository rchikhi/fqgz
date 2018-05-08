        
    // we're at the end of a sequence, i == the position of the \n
    // try to get to the beginning of a next one
    // assumes a few things:
    // header length only increases compared to the first measured header length
    // same for quality header length
    void skip_quality_and_header(unsigned &i, int readlen)
    {
        i ++; // skip that \n
        i += quality_header_length; // the quality + and the (likely empty) quality header
        i++; // go after the \n after quality header
        i += readlen;  // the quality seq
        i++; // go after the \n after quality seq
        if (header_ends_with_dna > 0)    
        {
            unsigned found_dna = 0;
            for (unsigned j = 0; j < header_length; j++)
                if (ascii2Dna[buffer[i++]] > 0) found_dna++; // go through header and see if we scan some dna
            if (found_dna < header_ends_with_dna)
                while (ascii2Dna[buffer[i++]] == 0 && i < size()) {} // go until we reach dna at end of header
            while (ascii2Dna[buffer[i++]] > 0 && i < size()) {} // go past dna at end of header
        }
        else
        {
            i += header_length; // the header
            // TODO go to next dna
        }
    }



and in parse_block:



        // just after "print whole block!"
        
        if (has_dummy_32k) // first block at a random access?
        {
            // in case this is the first block, we don't know where to start.
            // heuristic: go to the first stretch of min_read_length bases
            unsigned stretch = 0;
            while ((i < size()) && (stretch++) < min_read_length) 
            {
                if (ascii2Dna[buffer[i++]] == 0) stretch=0;
            }
            if ( i == size()) // no sequences
            {
                fprintf(stderr,"buffer doesnt contain any >= %d bp dna sequences", min_read_length);
                fully_reconstructed = false;
                return;            
            }
            i -= min_read_length;
        }


        // invariant: we're at the beginning of a DNA sequence in the fastq file
        while (i < size())
        {

            // let's see: are we really at the beginning of a dna seq?
            std::string buf_str = reinterpret_cast<const char *>(buffer); for (int j = 0; j < 40; j ++) if (buf_str[i-20+j] == '\n') buf_str[i-20+j] ='!';            fprintf(stderr,"parsing block, pos %6u: %s[>]%s\n",i,buf_str.substr(i-20,20).c_str(),buf_str.substr(i,20).c_str());
        

            if(stop != nullptr && last_block && stop->caught_up_first_seq(i - (current_blk - buffer)))
            {
                 fprintf(stderr,"reached first seq decoded by next thread\n");
                 break; // We reached the first sequence decoded by the next thread
            }

            while (ascii2Dna[buffer[i]] > 0 && i < size())
            {
                //fprintf(stderr,"parsing dna char %c, so far %s\n",c,current_sequence.c_str());
                current_sequence += buffer[i];
                i++;
            }
            
            if (ascii2Dna[buffer[i]] == 0 && i < size())
            {
                bool is_separator = is_likely_separator(i);
                if (is_separator && current_sequence.size() >= min_read_length /* shorter than that, it's definitely not a full read */)
                {
                    current_sequence_pos = i - current_sequence.size();
                    // Record the position of the first decoded sequence relative to the block start
                    // Note: this won't be used untill fully_reconstructed is turned on, so no risks of putting garbage here
                    first_seq_block_pos = current_sequence_pos - (current_blk - buffer); 

//#define DEBUG_SKIPPING
#ifdef DEBUG_SKIPPING
                    std::string buf_str = reinterpret_cast<const char *>(buffer);
                    for (int j = 0; j < 40; j ++) if (buf_str[i-20+j] == '\n') buf_str[i-20+j] ='!';
                    fprintf(stderr,"after parsing, prior to jump with read length %3u, pos %6u: %s[>]%s\n",(unsigned)current_sequence.size(),i,buf_str.substr(i-20,20).c_str(),buf_str.substr(i,20).c_str());
#endif
                    unsigned previous_i = i; 
                    skip_quality_and_header(i, current_sequence.size());

                    if (i >= size() && (!last_block)) // went past buffer, no chance to read more sequence
                    {
                        // lets not insert that sequence and keep it for the next block
                        previous_rewind = (size() - previous_i) + current_sequence.size(); // record how many chars to go back, in next block
                                                                                           //fprintf(stderr,"we think a good rewind location is -%d: %s[>]%s\n",
                                                                                           //previous_rewind,buf_str.substr(size()-previous_rewind-20,20).c_str(),
                                                                                           //buf_str.substr(size()-previous_rewind,std::min((unsigned)size()-20,(unsigned)20)).c_str());
                        break; 
                    }
                    else
                    {
                        //fprintf(stderr,"found separator after dna, have parsed read number %d: %s\n",(uint32_t)(putative_sequences.size()),current_sequence.c_str());
                        putative_sequences.push_back(current_sequence);
                        current_sequence = "";
                    }

#ifdef DEBUG_SKIPPING
                    for (int j = 0; j < 40; j ++) if (buf_str[i-20+j] == '\n') buf_str[i-20+j] ='!';
                    fprintf(stderr,"after parsing, after jump,                         pos %6u: %s[>]%s\n",i,buf_str.substr(i-20,20).c_str(),buf_str.substr(i,20).c_str());
#endif
                }
                else
                {
                    // if we have a fully solved buffer, should not have non-separator chars in seqs
                    // except in a pathological case where there is a ref from a header or quality
                    // test if it's of the form [dna]|[dna], if so, keep that seq for later
                    // 
                    if (fully_reconstructed)
                    {
                        handle_incomplete_context(i, current_sequence);
                    }
                    else
                    {
                        /*                            // when we're parsing DNA and notice that the next character isn't a \n, likely the context is incomplete
                                                      std::string prev = "none";
                                                      if (putative_sequences.size() > 0)
                                                      prev = putative_sequences[putative_sequences.size()-1];

                                                      std::string buf_str = reinterpret_cast<const char *>(buffer);
                                                      for (int j = 0; j < 40; j ++) if (buf_str[i-20+j] == '\n') buf_str[i-20+j] ='!';
                                                      int undetermined_counts = 0;
                                                      int nb_undetermined = 0;
                                                      for (int j = 0; j < 20; j++) if (buffer[i+j] == '|') { undetermined_counts+=buffer_counts[i+j]; nb_undetermined++;}
                                                      fprintf(stderr,"parsing error or incomplete context; %s[>]%s; mean undetermined count %.1f; cur seq %s; seq before: %s\n",buf_str.substr(i-20,20).c_str(),buf_str.substr(i,20).c_str(), 1.0*undetermined_counts/nb_undetermined, current_sequence.c_str(),prev.c_str());
                                                      */
                        incomplete_context = true;
                        break;
                    }
                    current_sequence = "";
                    i++;
                }
            }
            
        }

        if (previous_rewind == 0)
              previous_rewind = current_sequence.size();

