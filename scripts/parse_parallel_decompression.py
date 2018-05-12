import sys
from collections import defaultdict

if len(sys.argv) > 1:
    drive = sys.argv[1]
else:
    drive="hdd"
    drive="nvme"
    print("doing disk type=",drive)

from tinydb import TinyDB, Query
db = TinyDB('db.json')

skip_list = map(lambda x:x.strip(), open("skip_list").read().split())


def update(method, filename, time, nb_reads=0):
        global db
        time = int(time)
        if time == 0:
            return #not much to report here
        query = Query()
        search = db.search(query.filename.matches(".*"+filename))
        if len(search) == 0:
            # probably a small file
            return
        filename = search[0]['filename']
        size = int(search[0]['size'])
        speed = "%.1f" % (1.0*size/1024/1024/time)
        #print(filename,method,"%.2f" % speed,"MB/s")
        db.update({drive+"_"+method+'_speed' : speed}, query.filename == filename)
        if method=="gzip":
            db.update({"true_nb_reads": nb_reads}, query.filename == filename)
        elif "fqgz" in method:
            db.update({drive+"_"+method+'_nb_reads' : nb_reads}, query.filename == filename)

for method, res_file in [('gzip',drive+"/results-original-gunzip.txt"),('libdeflate',drive+"/results-modified-libdeflate.txt")]:
    for line in open(res_file):
        filename, nb_reads, time = line.split()
        if filename in skip_list: continue
        nb_reads = int(nb_reads)
        update(method ,filename, time, nb_reads=nb_reads)

for line in open(drive+"/results.txt"):
    filename, lines, linest4, linest8, linest12, time, timet4, timet8 = line.split()[:8]
    lines, linest4, linest8 = int(lines), int(linest4), int(linest8)
    if filename in skip_list: continue
    
    update("fqgz_1thread",filename, time, nb_reads=lines)
    update("fqgz_4thread",filename, timet4, nb_reads=linest4)
    update("fqgz_8thread",filename, timet8, nb_reads=linest8)

print("printing stats for lowest compression")
gzip_mean, libdeflate_mean, fqgz_t1_mean, fqgz_t4_mean = 0,0,0,0
count = 0
for f in db.all():
    if f['compression_level'] != "lowest": continue
    if drive+'_fqgz_1thread_speed' not in f: continue
    count += 1
    gzip_mean += float(f[drive+'_gzip_speed'])
    libdeflate_mean += float(f[drive+'_libdeflate_speed'])
    fqgz_t1_mean += float(f[drive+'_fqgz_1thread_speed'])
    fqgz_t4_mean += float(f[drive+'_fqgz_4thread_speed'])
    print(f['filename'],f[drive+'_gzip_speed'],f[drive+'_libdeflate_speed'],f[drive+'_fqgz_1thread_speed'],f[drive+'_fqgz_4thread_speed'])
gzip_mean /= count
libdeflate_mean /=count
fqgz_t1_mean /= count
fqgz_t4_mean /= count
l = [gzip_mean/gzip_mean, libdeflate_mean/gzip_mean,fqgz_t1_mean/gzip_mean,fqgz_t4_mean/gzip_mean]
print([ gzip_mean * x for x in l])
