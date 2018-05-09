f=~/data/gzip/medium_fastq.fq.gz
./gunzip -c $f > /tmp/mine
gunzip -c $f | awk 'NR%4==2' > /tmp/original
diff /tmp/mine /tmp/original

