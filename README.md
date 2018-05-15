This is `gunzip` except that there is a new `-t [int]` parameter that controls the number of threads, and you can only use it to decompress FASTQ files ¯\\\_(ツ)\_/¯


## Installing

Type `make` and use it exactly as gunzip

## Limitations

Cannot compress, only decompress. In some files with normal/high compression levels, the program will not return all sequences.
