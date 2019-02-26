# pugz

Parallel decompression of gzipped text files.

Proof of concept: decompresses everything, but does not print anything (for the sake of speed).

*Note:* Currently, decompression can handle files as large as (500 MB) * number of threads, i.e. 2 GB for 4 threads. 
This is because the current implementation decompreses the whole file at once, loaded in memory.
Larger files will need to be processed in chunks (still exact decompression, still in parallel), which is an implementation 'detail' that we will release soon.

## Getting Started

### Installing

Type:

```
make asserts=0
```

### Test

We provide a small example:

```
cd example
bash test.sh
``` 

## License

This project is licensed under the MIT License.

## Citation 

* Random access to gzip-compressed FASTQ files, in submission

## Acknowledgements

[ebiggers](https://github.com/ebiggers) for writing [libdeflate](https://github.com/ebiggers/libdeflate)


