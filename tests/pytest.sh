#!/bin/sh

if [ -z $PYTHON ]; then # prefer env setting if available
    if [ ! -z $(which python27) ]; then
        PYTHON=$(which python27)
    elif [ ! -z $(which python2.7) ]; then
        PYTHON=$(which python2.7)
    elif [ ! -z $(which python26) ]; then
        PYTHON=$(which python26)
    elif [ ! -z $(which python2.6) ]; then
        PYTHON=$(which python2.6)
    else
        PYTHON=$(which python)
    fi
fi

echo "python binary chosen: $PYTHON"

if expr match "$PWD" ".*twemcache" = 0 >/dev/null; then
    echo "Run this script from the twemcache directory"
    exit 1
fi

if [ -f $PWD/src/twemcache ]; then
    echo "Running tests against your local binary at $PWD/src/twemcache"
else
    echo "No local binary of twemcache found in $PWD/src, run 'make' first?"
    exit 1
fi

INSTANCES=`pgrep emcache` # covers twemcache, twemcached and memcached
for ins in $INSTANCES;
do
    echo "a cache instance (pid = $ins) is likely to be running and may conflict with test target. Proceed?[y/N] "
    read RES
    if [ "$RES" != "y" ]; then
        echo "aborting test"
        exit
    fi
done

# if PYTHONTEST does have the tests path, add it
if expr match "$PYTHONPATH" ".*$PWD/tests" = 0 >/dev/null; then
    echo "Adding /tests directory to PYTHONPATH"
    export PYTHONPATH=$PWD/tests:$PYTHONPATH
fi

config_base=$PWD/tests/config/defaults.py
config_base_template=$PWD/tests/config/defaults-template.py
config_server=$PWD/tests/config/server/default.py
config_server_template=$PWD/tests/config/server/default-template.py
config_data=$PWD/tests/config/data/default.py
config_data_template=$PWD/tests/config/data/default-template.py

data_default=$PWD/tests/data/data.default # only used in performance tests
data_default_template=$PWD/tests/data/data-template.default


# make sure we have all the config files needed
if [ -f $config_base ]; then
    echo "We will use the existing default *base* config file at $config_base"
else
    if [ -f $config_base_template ]; then
        echo "Creating the default *base* config file with template"
        cp $config_base_template $config_base
    else
        echo "Template missing for $config_base. git pull again?"
        exit 1
    fi
fi

if [ -f $config_server ]; then
    echo "We will use the existing default *server* config file at $config_server"
else
    if [ -f $config_server_template ]; then
        echo "Creating the default *server* config file with template"
        # set the twemcache binary we are testing to the one in current directory
        sed "s:EXEC = 'twemcache':EXEC = '$PWD/src/twemcache':" <$config_server_template >$config_server
    else
        echo "Template missing for $config_server. git pull again?"
        exit 1
    fi
fi

if [ -f $config_data ]; then
    echo "We will use the existing default *data* config file at $config_data"
else
    if [ -f $config_data_template ]; then
        echo "Creating the default *data* config file with template"
        cp $config_data_template $config_data
    else
        echo "Template missing for $config_data. git pull again?"
        exit 1
    fi
fi

if [ -f $data_default ]; then
    echo "We will use the existing default data file at $data_default"
else
    if [ -f $data_default_template ]; then
        echo "Creating the default data file with template"
        cp $data_default_template $data_default
    else
        echo "Template missing for $data_default. git pull again?"
        exit 1
    fi
fi

echo "Firing off tests now"

$PYTHON $PWD/tests/testrunner.py $* 2>/dev/null
