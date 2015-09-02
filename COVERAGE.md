# Twemcache Test Coverage

To generate test coverage using gcov & lcov.

## Requirement
Have both gcov and lcov installed.

## Build

Configure Twemcache with no optimization and with gcov enabled
```sh
    CFLAGS="-ggdb3 -O0" ./configure --enable-debug=full --enable-gcov
```

## Collect data (running at project root level)
```sh
    lcov -c --no-external --rc lcov_branch_coverage=1 -i -b src/ -d src/ -o twemcache_base.info
    make test
    lcov -c --no-external --rc lcov_branch_coverage=1 -b src/ -d src/ -o twemcache_test.info
    lcov -a twemcache_base.info -a twemcache_test.info --rc lcov_branch_coverage=1 -o twemcache_total.info
    genhtml --ignore-errors source twemcache_total.info --legend --output-directory=/tmp/twemcache
```
