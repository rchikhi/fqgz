import sys
from collections import defaultdict

if len(sys.argv) > 1:
    drive = sys.argv[1]
else:
    drive="hdd"
    drive="nvme"
    print("doing disk type=",drive)

from tinydb import TinyDB, Query
db = TinyDB(drive+'_db.json')

def update(method, filename, time):
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
        db.update({method+'_speed' : speed}, query.filename == filename)

for method, res_file in [('gzip',drive+"/results-original-gunzip.txt"),('libdeflate',drive+"/results-modified-libdeflate.txt")]:
    for line in open(res_file):
        filename, lines, time = line.split()
        lines = int(lines)
        update(method ,filename, time)

for line in open(drive+"/results.txt"):
    filename, lines, linet4, linest8, linest12, time, timet4, timet8 = line.split()[:8]
    
    update("fqgz_1thread",filename, time)
    update("fqgz_4thread",filename, timet4)
    update("fqgz_8thread",filename, timet8)

print("printing stats for lowest compression")
gzip_mean, libdeflate_mean, fqgz_t1_mean, fqgz_t4_mean = 0,0,0,0
count = 0
for f in db.all():
    if f['compression_level'] != "lowest": continue
    if 'fqgz_1thread_speed' not in f: continue
    count += 1
    gzip_mean += float(f['gzip_speed'])
    libdeflate_mean += float(f['libdeflate_speed'])
    fqgz_t1_mean += float(f['fqgz_1thread_speed'])
    fqgz_t4_mean += float(f['fqgz_4thread_speed'])
    print(f['filename'],f['gzip_speed'],f['libdeflate_speed'],f['fqgz_1thread_speed'],f['fqgz_4thread_speed'])
gzip_mean /= count
libdeflate_mean /=count
fqgz_t1_mean /= count
fqgz_t4_mean /= count
l = [gzip_mean/gzip_mean, libdeflate_mean/gzip_mean,fqgz_t1_mean/gzip_mean,fqgz_t4_mean/gzip_mean]
print([ gzip_mean * x for x in l])
