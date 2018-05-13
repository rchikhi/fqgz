#for f in `ls /nvme/fastq/ERA*.gz`
for f in `ls /home/gzip/fastq/hdd_files/ERA*.gz`
#for f in `cat list_lowest`
#for f in "$HOME/fastq/hdd_files/ERA969556-CAGE.1A.BC.CGA.GM12814.fq.gz"  # do it for an extra file
do
    f=$(readlink -f $f)
    FILESIZE=$(stat -c%s "$f")
    if [ "$FILESIZE" -lt "200000000" ]
    then
        #echo "$f too small"
        continue
    fi
    for offset in $(($FILESIZE*1/2)) $(($FILESIZE*1/4)) $(($FILESIZE*2/3)) $(($FILESIZE*1/3))
    do
        echo "$(file $f) size $FILESIZE seek at $offset"
        (./gunzip -s $offset -c $f 3>&1 1>&2- 2>&3-) 2>/dev/null
    done
done
