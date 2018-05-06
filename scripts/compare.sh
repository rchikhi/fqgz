./gunzip -c small_fastq.fq.gz > mine
gunzip -c small_fastq.fq.gz | awk 'NR%4==2' > original
diff mine original

