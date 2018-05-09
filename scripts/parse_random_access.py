import sys
from collections import defaultdict
filename, compression_mode, nb_reads, nb_ambiguous,size = "","",0,0,0
table = defaultdict(list)
def record():
    #success = 1 if (nb_reads > 0 and nb_ambiguous == 0) else 0 
    success = ((1.0*nb_reads)/(nb_reads+nb_ambiguous)) if nb_reads > 0 else 0
    table[compression_mode]+=[(filename,size,success)]
    print(filename,compression_mode,size,nb_reads,nb_ambiguous,success)
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
    if "done, printed" in line:
        nb_reads = int(line.split()[2])

    if "and also didn't print" in line:
        nb_ambiguous = int(line.split()[4])
    else:
        nb_ambiguous = 0
record()
print("----")
all_sum = 0
all_success = 0
nb_files = 0
for compression_mode in table:
    sum_size = 0
    avg_success = 0
    for (filename, size, success) in table[compression_mode]:
        sum_size += size
        avg_success += success

        all_sum += size
        all_success += success
        nb_files += 1
    avg_success *= 100.0/len(table[compression_mode])
    sum_size /= 1024*1024*1024.0
    print(compression_mode,len(table[compression_mode]),sum_size,avg_success)

all_sum /= 1024*1024*1024.0
all_success *= 100.0/nb_files
print("Total",nb_files,all_sum,all_success)
