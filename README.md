This is `gunzip` except that there is a new `-t [int]` parameter that controls the number of threads, and you can only use it to decompress FASTQ files ¯\\\_(ツ)\_/¯


## Installing

Type `make` and 

## Usage 

Exactly as gunzip. Two new parameters:

* `-t` controls the number of threads

* `-s XXX` skip XXX bytes in the compressed file and start decompression from this offset. In practice, will start a little bit later.

## Limitations

Cannot compress, only decompress. In some files with normal/high compression levels, the program will not return all sequences.
