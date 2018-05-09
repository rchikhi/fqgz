f=~/data/gzip/medium_fastq.fq.gz
time ../libdeflate-me/gunzip -c -t 3 $f |sort > /tmp/mine
time gunzip -c $f | awk 'NR%4==2' |sort> /tmp/original
diff /tmp/mine /tmp/original

