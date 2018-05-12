#custom script to crawl ENA and get one FASTQ.gz file for all recent experiments

import os
from ftplib import FTP
from datetime import datetime

ftp = FTP("ftp.sra.ebi.ac.uk")
ftp.login()

ftp.cwd('vol1')

def process_listing(dirs): # in place
    for i in range(len(dirs)):
        line = dirs[i].split()
        dt = line[-4:-1]
        if ":" in dt[2] :
            if dt[0] in ["Nov","Dec"]: # hack but i'm hurry
                dt[2]="2017"
            else:
                dt[2] = "2018"
        try:
            dt = datetime.strptime(' '.join(dt), '%b %d %Y')    
        except:
            print("bad date",dt)
            dt = None
        folder = line[-1]
        dirs[i] = (dt,folder)

dirs =  []
ftp.retrlines('LIST', lambda line : dirs.append(line) if "ERA" in line else None)

process_listing(dirs)

# so actually folder date is maybe not the best indicator, lets just process by recent ERA IDs
#list_folders = sorted(dirs)[::-1]
list_folders = sorted(dirs,key = lambda x:x[1])[::-1]

#print(list_folders)

for line in list_folders:
    print(line)
    ftp.cwd(line[1])
    folders = []
    ftp.retrlines('LIST', lambda line: folders.append(line.split()[-1]) if "ERA" in line else None)
    print(folders)
    for folder in folders:
        print("exploring",line[1],'/',folder)
        ftp.cwd(folder)
        has_fastq = []
        ftp.retrlines('LIST', lambda line: has_fastq.append(True) if "fastq" == line.split()[-1] else None)
        if True in has_fastq:
            ftp.cwd('fastq')
            files = []
            ftp.retrlines('LIST', lambda line : files.append(line) if "gz" in line else None)
            process_listing(files)
            #print(files)
            if len(files) != 0:
                file = files[0][-1] # arbitrary
                local_filename = folder+'-'+file
                if os.path.exists(local_filename):
                    print("skipping existing",local_filename)
                else:
                    print("downloading",local_filename)
                    try:
                        ftp.retrbinary("RETR " + file ,open(local_filename, 'wb').write)
                    except:
                        print "Error"

            ftp.cwd('..')
        ftp.cwd('..')
    ftp.cwd('/vol1')
