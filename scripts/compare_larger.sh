f=~/data/medium_fastq.fq.gz
./gunzip -c $f > mine
gunzip -c $f | awk 'NR%4==2' > original
diff mine original

