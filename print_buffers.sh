(./gunzip -c -s 100000000 $1 3>&1 1>&2- 2>&3-) 2>/dev/null | grep "block.*0: " -A 5 |  cut -c -1000 | less
