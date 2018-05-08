import os
import glob
import gzip
import readfq
for filename in glob.glob("/nvme/fastq/*"):
    print("---------")
    print(filename)
    #os.system("zcat %s |head -n 4" % filename)
    count = 0
    len_seqs = []
    seq_in_headers = 0
    for name, seq, qual in readfq.readfq(gzip.open(filename)): 
        if any([name.strip().endswith(x) for x in 'ACTGN']):
            seq_in_headers += 1
        len_seqs.append(len(seq))
        if count > 10000:
            break
        count = count + 1
    if len(set(len_seqs)) == 1:
        print("all same length")
    if seq_in_headers > 5000:
        print("header ends with sequence")
    if len(set(len_seqs)) > 1 and seq_in_headers:
        print("XXXXXX the type we don't support")
