def parse_time_file(filepath):
    runs = []
    cur_run = []
    for l in open(filepath):
        l = l.rstrip()
        cur_run.append(l)
        if 'Exit status:' in l:
            if l.endswith('1'):
                cur_run.pop(-24)

            fields = {}
            for l in cur_run[-23:]:
                assert l[0] == '\t'
                key, _, value = l[1:].partition(': ')

                if key == 'Elapsed (wall clock) time (h:mm:ss or m:ss)':
                    key='WCT'
                    t = 0
                    for tfrag in value.split(':'):
                        t *= 60
                        t += float(tfrag)
                    value = t

                fields[key] = value

            if fields['Exit status'] != '0':
                fields['stderr'] = '\n'.join(cur_run[:-24])
            else:
                fields['stderr'] = '\n'.join(cur_run[:-23])

            runs.append(fields)

            cur_run = []
    return runs

def detect_multiparts(runs, fq_dir = '/nvme/fastq/'):
    bad_fqs = []

    for r in runs:
        cmd = r['Command being timed'][1:-1]
        _, _, fq_file = cmd.rpartition(' '+fq_dir)
        if 'Decompressed size % 4GB (' in r['stderr']:
            #print(r['stderr'])
            bad_fqs.append(fq_file)
    return bad_fqs


runs = parse_time_file('/home/gzip/benchmark/results_bench_modified_v0.8.txt')
print(' '.join(detect_multiparts(runs, '/home/gzip/fastq/hdd_files/')))
