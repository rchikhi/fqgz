#just make sure that fastq's header lines are no shorter than the first header line
import gzip
import sys
with gzip.open(sys.argv[1]) as f:
    header = f.readline().decode().strip()
    counter = 0
    hs = len(header)
    for line in f:
        line = line.decode().strip()
        if line.startswith('@'):
            if len(line) < hs:
                print(len(line),line,"hs",hs)
            counter += 1
            if counter % 100000 == 0:
                print("line",counter,"header len",len(line))
