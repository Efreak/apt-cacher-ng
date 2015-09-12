ACTMP=$1
shift
exe=$1
shift
PORT=14981
while netstat -antup 2>&1 | grep $PORT ; do PORT=$(( $PORT + 1 )) ; done
$exe port:$PORT cachedir=$ACTMP logdir:$ACTMP foreground:0 pidfile=$ACTMP/_pid

finish_acng() {
  kill $(cat $ACTMP/_pid)
}

export http_proxy=http://localhost:$PORT
export https_proxy=http://localhost:$PORT
