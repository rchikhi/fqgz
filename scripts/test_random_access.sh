#for f in `ls /nvme/fastq/ERA*.gz`
for f in `ls /home/gzip/fastq/hdd_files/ERA*.gz`
do
    f=$(readlink -f $f)
    FILESIZE=$(stat -c%s "$f")
    if [ "$FILESIZE" -lt "200000000" ]
    then
        #echo "$f too small"
        continue
    fi
    offset=$(($FILESIZE*1/2))
    echo "$(file $f) size $FILESIZE seek at $offset"
    (./gunzip -s $offset -c $f 3>&1 1>&2- 2>&3-) 2>/dev/null
done
