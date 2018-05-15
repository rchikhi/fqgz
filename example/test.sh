#!/bin/bash
if  [ ! -f docword.nytimes.txt.gz ]; then
    wget https://s3-eu-west-1.amazonaws.com/artm/docword.nytimes.txt.gz
fi
echo "testing with system's gunzip"
time gunzip -c docword.nytimes.txt.gz  >/dev/null
echo "testing with pugz, 1 thread"
time ../gunzip -c docword.nytimes.txt.gz  >/dev/null
echo "testing with pugz, 8 threads"
time ../gunzip -t 8 -c docword.nytimes.txt.gz  >/dev/null
