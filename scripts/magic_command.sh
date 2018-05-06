# swaps stderr and stdout
./gunzip  -c $1  3>&1 1>&2 2>&3 1>/dev/null| less
