#!/bin/bash -e

## Starts or stops twemcached
## Example script usage: ./twemcached start
##
## @note - Supported Platforms centos

set -e

SERVICE="twemcached"

. startup_includes

pushd ${SERVICE_HOME}

COMMAND="sh ./start -Dpidfile=${PIDFILE}"

echo $COMMAND
start(){
  pushd ${SERVICE_HOME}
  echo starting ${SERVICE}
  echo logging output to ${LOGFILE}
  echo ${COMMAND}
  ${COMMAND} < /dev/null 2>&1 | tee -a ${LOGFILE} & disown
  popd
}

stop(){
  pushd ${SERVICE_HOME}
  echo stopping ${SERVICE}
  echo pidfile is ${PIDFILE}
  kill -9 `cat ${PIDFILE}` && rm ${PIDFILE}
  popd
}

case "$1" in
      start)
        start
        ;;
      stop)
        stop
        ;;
      *)
        echo $"Usage: $0 {start|stop}"
        exit 1
esac

popd
