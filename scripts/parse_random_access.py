import numpy as np
import os
import sys
from collections import defaultdict
filename, compression_mode, nb_reads, nb_ambiguous, size, nb_skipped_bytes = "","",0,0,0,-1
table = defaultdict(list)
def record():
    if not os.path.exists("/home/gzip/fastq/hdd_files/"+os.path.basename(filename)):
        print("not recording stats for file",os.path.basename(filename),"might be multipart/blocked")
        return
    #success = 1 if (nb_reads > 0 and nb_ambiguous == 0) else 0 
    success = ((1.0*nb_reads)/(nb_reads+nb_ambiguous)) if nb_reads > 0 else 0
    table[compression_mode]+=[(filename,size,nb_skipped_bytes,success)]
    print(filename,compression_mode,size,nb_reads,nb_ambiguous,success,nb_skipped_bytes)

for line in open(sys.argv[1]):
    if ".gz" in line:
       if filename != "": record()
       filename = line.split(":")[0]
       if "max speed" in line:
            compression_mode = "lowest"
       elif "max compression" in line:
            compression_mode = "best"
       else:
            compression_mode = "normal"
       if filename.endswith("ERA987833-CNC_CasAF3_CRI1strepeat_rep1_R1.fastq.gz"):
           compression_mode = "best" # clearly not a gzip -1, it compresses even better than gzip -9, so maybe zopfli
       size = int(line.split()[line.split().index("seek")-1])
       nb_reads, nb_ambiguous, nb_skipped_bytes = 0,0,-1
    if "done, printed" in line:
        nb_reads = int(line.split()[2])
    if "and also didn't print" in line:
        nb_ambiguous = int(line.split()[4])
    else:
        nb_ambiguous = 0
    if "at decoded block" in line and len(line.split()) > 13:
        nb_blocks, mean_block_length = int(line.split()[9].strip(',')), float(line.split()[13])
        nb_skipped_bytes = nb_blocks * mean_block_length

record()

def avg_sd(l,norm=0,perc=0):
    assert(type(l) == list and len(l)>1)
    return ("%5.1f" % ((100.0**perc)*np.average(l)/(1024**norm))) + " +- " + ("%2.3f" % ((100.0**perc)*np.std(l)/(1024**norm)))

print("----")
print("%10s" % "comp", "%5s" % "files","%5s"  % "size", "%12s" % "skipped", "%12s" % "success")
all_sizes, all_successes, all_skipped_bytes = [],[],[]
for compression_mode in table:
    sizes, successes, skipped_bytes = [], [], []
    for (filename, size, skipped, success) in table[compression_mode]:
        sizes.append(size)
        successes.append(success)
        if skipped != -1:
            skipped_bytes.append(skipped)

    if  len(skipped_bytes) > 0 :
        avg_skipped_bytes = avg_sd(skipped_bytes,2)
    else:
        avg_skipped_bytes = "NA"
    all_sizes.extend(sizes)
    all_successes.extend(successes)
    all_skipped_bytes.extend(skipped_bytes)
    print("%10s" % compression_mode, "%5d" % len(table[compression_mode]),"%5.1f" % (sum(sizes)/(1024*1024*1024.0)),  avg_skipped_bytes, avg_sd(successes,perc=1))

print("%10s" % "Total", "%5d" % len(all_sizes), "%5.1f" % (sum(all_sizes)/(1024*1024*1024.0)),avg_sd(all_skipped_bytes,norm=2), avg_sd(all_successes,perc=1))

# save to tinydb
from tinydb import TinyDB, Query
db = TinyDB('db.json')
for compression_mode in table:
    for (filename, size, nb_skipped_bytes, success) in table[compression_mode]:
        db.insert({'filename': filename, 'compression_level': compression_mode, 'size' : size, 'nb_skipped_bytes': nb_skipped_bytes })
