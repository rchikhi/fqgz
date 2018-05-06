f=~/data/medium_fastq.fq.gz
time ../libdeflate-me/gunzip -c -t 3 $f |sort > mine
time gunzip -c $f | awk 'NR%4==2' |sort> original
diff mine original

