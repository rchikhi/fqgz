This is `gunzip` except that there is a new `-t [int]` parameter that controls the number of threads, and you can only use it to decompress FASTQ files ¯\\\_(ツ)\_/¯


## Installing

Type `make`

## Usage 

Exactly same as gunzip. Two new parameters:

* `-t [n]` use `n` threads

* `-s [p]` skip `p` bytes in the compressed file and start decompression as soon as a unambiguous block is found.

Those two parameters are mutually exclusive, `-s` can only decompress at a location using 1 thread.

## Limitations

Cannot compress, only decompress. In some files with normal/high compression levels, the program will not return all sequences, but will also tell you how many sequences were not returned.

## Citation

Random access to sequences in gzip-compressed FASTQ files, submitted

## Reproducibility

We have included a few scripts that were used to create the results from the paper.

* `paper/list_files`: list of 100 ENA files
* `scripts/test_random_access.sh`: script that runs fqgz to obtain data for Table 1
* `scripts/parse_random_access.py`: script that creates Table 1 from the output of the above script

