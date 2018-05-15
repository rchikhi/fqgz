This is `gunzip` except that there is a new `-t [int]` parameter that controls the number of threads, and you can only use it to decompress FASTQ files ¯\\\_(ツ)\_/¯


## Installing

Type `make`

## Usage 

Exactly as gunzip. Two new parameters:

* `-t [n]` use `n` threads

* `-s [p]` skip `p` bytes in the compressed file and start decompression as soon as a unambiguous block is found.

## Limitations

Cannot compress, only decompress. In some files with normal/high compression levels, the program will not return all sequences.
