trap 'exit 130' INT

mv results.txt results.txt.bak
rm -f results-original-gunzip.txt results-modified-libdeflate.txt
for f in `ls /nvme/fastq/*.gz`
do
    echo $f
    base=$(basename $f)

    do_other_methods=1
    if [ "$do_other_methods" == 1 ]
    then
        start=`date +%s`
        clear_cache
        truth=$(time gunzip -c $f | wc -l)
        end=`date +%s`
        truth=$((truth/4))
        runtime_truth=$((end-start))
        echo "$base $truth $runtime_truth" >> results-original-gunzip.txt

        start=`date +%s`
        clear_cache
        modified=$(time ../modified_v0.8/gunzip -k -c $f | wc -l)
        end=`date +%s`
        runtime_modified=$((end-start))
        modified=$((modified/4))
        echo "$base $modified $runtime_modified" >> results-modified-libdeflate.txt
    else
        start=`date +%s`
        clear_cache
        ours=$(time ./gunzip -c $f 2>/dev/null |wc -l)
        end=`date +%s`
        runtime_ours=$((end-start))
        
        start=`date +%s`
        clear_cache
        ourst4=$(time ./gunzip -c -t 4 $f 2>/dev/null |wc -l)
        end=`date +%s`
        runtime_ourst4=$((end-start))

        start=`date +%s`
        clear_cache
        ourst8=$(time ./gunzip -c -t 8 $f 2>/dev/null |wc -l)
        end=`date +%s`
        runtime_ourst8=$((end-start))

        start=`date +%s`
        clear_cache
        ourst12=$(time ./gunzip -c -t 12 $f 2>/dev/null |wc -l)
        end=`date +%s`
        runtime_ourst12=$((end-start))

        echo "$base $truth $ours $ourst4 $ourst8 $ourst12 $runtime_truth $runtime_ours $runtime_ourst4 $runtime_ourst8 $runtime_ourst12" >> results.txt
    fi
done
