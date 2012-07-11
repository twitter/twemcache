## Random eviction

Random eviction enables twemcache to gracefully move on-the-fly from one slab class to another for an application's workload. The candidate slab is chosen randomly, on-demand from all the active slabs. Since, we maintain an append-only slabtable for every allocated slab in twemcache, this choice itself is O(1). Evicting a slab would require us to evict all the items in the slab which is O(items_per_slab).

The adaptability of twemcache with random eviction can easily be seen in the experiment below. In this experiment, we spawn a cache server with a maximum memory capacity of 64MB and subject it to a series of test runs. By default, every slab is of size 1MB and hence a cache instance with a memory capacity of 64MB would allocate at most 64 slabs. In each test run, we generate a 'set' request load of 100MB and record the slab distribution across different slab classes. Since we are generating a load that exceeds the memory capacity of the cache, eviction would be triggered in every test run. Furthermore, we ensure that every test run generates requests that differ significantly in size in comparison to it's previous test run, which would ensure that requests in a given run get mapped to a different, unseen slab class.

Test runs:

1. 10K requests of size **100 bytes** generated over 100 connections.
2. 1K requests of size **1K bytes** generated over 100 connections.
3. 100 requests of size **10K bytes** generated over 100 connections.
4. 10 requests of size **100K bytes** generated over 100 connections.

Results and slab distribution with the test runs:

memcached v1.4.13 with slab_reassign,slab_automove:

    +---------------------------------------------------------+
    | size | calls |  slab  |  slab  | slab  | slab  | server |
    |      |       |   100  |   1K   |  10K  | 100K  | error  |
    +---------------------------------------------------------+
    | 100  |  10K  |   64   |   0    |   0   |   0   |   0    |
    +---------------------------------------------------------+
    |  1K  |  1K   |   64   |   1    |   0   |   0   |   0    |
    +---------------------------------------------------------+
    | 10K  |  100  |   64   |   1    |   1   |   0   |   0    |
    +---------------------------------------------------------+
    | 100K |  10   |   64   |   1    |   1   |   1   |  883   |
    +---------------------------------------------------------+

twemcache v2.4.0 with --eviction-strategy=2:

    +---------------------------------------------------------+
    | size | calls |  slab  |  slab  | slab  | slab  | server |
    |      |       |   100  |   1K   |  10K  | 100K  | error  |
    +---------------------------------------------------------+
    | 100  |  10K  |   64   |   0    |   0   |   0   |   0    |
    +---------------------------------------------------------+
    |  1K  |  1K   |   15   |   49   |   0   |   0   |   0    |
    +---------------------------------------------------------+
    | 10K  |  100  |   6    |   4    |   54  |   0   |   0    |
    +---------------------------------------------------------+
    | 100K |  10   |   2    |   1    |   10  |   51  |   0    |
    +---------------------------------------------------------+

With random eviction, twemcache adapts on-demand to the application's active working set of items. However, with memcached, slabs allocated in the first run to slab-100 class are locked in and hence not available to requests generated in the subsequent runs.

## Details

### memcached v1.4.13

    $ memcached -o slab_reassign,slab_automove

    $ printf "stats settings\r\n" | nc localhost 11211 | grep 'move\|assign'
    STAT slab_reassign yes
    STAT slab_automove yes

#### 10K requests, 100 byte size, 100 conns, 100MB data

    $ ./mcperf --num-conns=100 --conn-rate=1000 --sizes=0.01 --num-calls=10000

    Total: connections 100 requests 1000000 responses 1000000 test-duration 81.696 s

    Connection rate: 1.2 conn/s (817.0 ms/conn <= 100 concurrent connections)
    Connection time [ms]: avg 81581.8 min 81557.8 max 81600.3 stddev 11.32
    Connect time [ms]: avg 0.1 min 0.0 max 0.7 stddev 0.16

    Request rate: 12240.5 req/s (0.1 ms/req)
    Request size [B]: avg 129.0 min 129.0 max 129.0 stddev 0.00

    Response rate: 12240.5 rsp/s (0.1 ms/rsp)
    Response size [B]: avg 8.0 min 8.0 max 8.0 stddev 0.00
    Response time [ms]: avg 8.2 min 1.0 max 143.1 stddev 0.00
    Response time [ms]: p25 7.0 p50 8.0 p75 10.0
    Response time [ms]: p95 12.0 p99 14.0 p999 18.0
    Response type: stored 1000000 not_stored 0 exists 0 not_found 0
    Response type: num 0 deleted 0 end 0 value 0
    Response type: error 0 client_error 0 server_error 0

    Errors: total 0 client-timo 0 socket-timo 0 connrefused 0 connreset 0
    Errors: fd-unavail 0 ftab-full 0 addrunavail 0 other 0

    CPU time [s]: user 25.95 system 41.49 (user 31.8% system 50.8% total 82.5%)
    Net I/O: bytes 130.7 MB rate 1637.6 KB/s (13.4*10^6 bps)

    $ printf "stats slabs\r\n" | nc localhost 11211 | grep "total_pages"
    STAT 4:total_pages 64

#### 1K requests, 1000 byte size, 100 conns, 100MB data

    $ ./mcperf --num-conns=100 --conn-rate=1000 --sizes=0.001 --num-calls=1000

    Total: connections 100 requests 100000 responses 100000 test-duration 8.234 s

    Connection rate: 12.1 conn/s (82.3 ms/conn <= 100 concurrent connections)
    Connection time [ms]: avg 8136.9 min 8128.8 max 8147.4 stddev 4.78
    Connect time [ms]: avg 0.1 min 0.0 max 1.7 stddev 0.22

    Request rate: 12144.3 req/s (0.1 ms/req)
    Request size [B]: avg 1030.0 min 1030.0 max 1030.0 stddev 0.00

    Response rate: 12144.3 rsp/s (0.1 ms/rsp)
    Response size [B]: avg 8.0 min 8.0 max 8.0 stddev 0.00
    Response time [ms]: avg 8.1 min 0.4 max 20.7 stddev 0.00
    Response time [ms]: p25 6.0 p50 8.0 p75 10.0
    Response time [ms]: p95 12.0 p99 14.0 p999 16.0
    Response type: stored 100000 not_stored 0 exists 0 not_found 0
    Response type: num 0 deleted 0 end 0 value 0
    Response type: error 0 client_error 0 server_error 0

    Errors: total 0 client-timo 0 socket-timo 0 connrefused 0 connreset 0
    Errors: fd-unavail 0 ftab-full 0 addrunavail 0 other 0

    CPU time [s]: user 2.68 system 4.20 (user 32.5% system 51.0% total 83.5%)
    Net I/O: bytes 99.0 MB rate 12310.3 KB/s (100.8*10^6 bps)

    $ printf "stats slabs\r\n" | nc localhost 11211 | grep "total_pages"
    STAT 4:total_pages 64
    STAT 12:total_pages 1

#### 100 requests, 10000 byte size, 100 conns, 100MB data

    $ ./mcperf --num-conns=100 --conn-rate=1000 --sizes=0.0001 --num-calls=100

    Total: connections 100 requests 10000 responses 10000 test-duration 0.318 s

    Connection rate: 314.7 conn/s (3.2 ms/conn <= 100 concurrent connections)
    Connection time [ms]: avg 247.2 min 175.4 max 307.9 stddev 31.12
    Connect time [ms]: avg 0.2 min 0.0 max 2.1 stddev 0.39

    Request rate: 31469.6 req/s (0.0 ms/req)
    Request size [B]: avg 10031.0 min 10031.0 max 10031.0 stddev 0.00

    Response rate: 31469.6 rsp/s (0.0 ms/rsp)
    Response size [B]: avg 8.0 min 8.0 max 8.0 stddev 0.00
    Response time [ms]: avg 2.5 min 0.0 max 23.2 stddev 0.00
    Response time [ms]: p25 1.0 p50 1.0 p75 1.0
    Response time [ms]: p95 13.0 p99 18.0 p999 23.0
    Response type: stored 10000 not_stored 0 exists 0 not_found 0
    Response type: num 0 deleted 0 end 0 value 0
    Response type: error 0 client_error 0 server_error 0

    Errors: total 0 client-timo 0 socket-timo 0 connrefused 0 connreset 0
    Errors: fd-unavail 0 ftab-full 0 addrunavail 0 other 0

    CPU time [s]: user 0.03 system 0.14 (user 10.1% system 42.8% total 52.9%)
    Net I/O: bytes 95.7 MB rate 308518.7 KB/s (2527.4*10^6 bps)

    $ printf "stats slabs\r\n" | nc localhost 11211 | grep "total_pages"
    STAT 4:total_pages 64
    STAT 12:total_pages 1
    STAT 22:total_pages 1

#### 10 requests, 100000 byte size, 100 conns, 100MB data

    $ ./mcperf --num-conns=100 --conn-rate=1000 --sizes=0.00001 --num-calls=10

    Total: connections 100 requests 1000 responses 1000 test-duration 0.499 s

    Connection rate: 200.5 conn/s (5.0 ms/conn <= 100 concurrent connections)
    Connection time [ms]: avg 211.1 min 160.3 max 454.7 stddev 44.42
    Connect time [ms]: avg 0.2 min 0.0 max 2.1 stddev 0.40

    Request rate: 2005.1 req/s (0.5 ms/req)
    Request size [B]: avg 100032.0 min 100032.0 max 100032.0 stddev nan

    Response rate: 2005.1 rsp/s (0.5 ms/rsp)
    Response size [B]: avg 38.9 min 8.0 max 43.0 stddev 11.26
    Response time [ms]: avg 20.8 min 0.1 max 207.6 stddev 0.02
    Response time [ms]: p25 10.0 p50 11.0 p75 20.0
    Response time [ms]: p95 56.0 p99 92.0 p999 100.0
    Response type: stored 117 not_stored 0 exists 0 not_found 0
    Response type: num 0 deleted 0 end 0 value 0
    Response type: error 0 client_error 0 server_error 883

    Errors: total 0 client-timo 0 socket-timo 0 connrefused 0 connreset 0
    Errors: fd-unavail 0 ftab-full 0 addrunavail 0 other 0

    CPU time [s]: user 0.06 system 0.23 (user 12.8% system 45.7% total 58.6%)
    Net I/O: bytes 95.4 MB rate 195945.9 KB/s (1605.2*10^6 bps)

    $ printf "stats slabs\r\n" | nc localhost 11211 | grep "total_pages"
    STAT 4:total_pages 64
    STAT 12:total_pages 1
    STAT 22:total_pages 1
    STAT 33:total_pages 1


### twemcache v2.4.0

    $ twemcache

#### 10K requests, 100 byte size, 100 conns, 100MB data

    $ ./mcperf --num-conns=100 --conn-rate=1000 --sizes=0.01 --num-calls=10000

    Total: connections 100 requests 1000000 responses 1000000 test-duration 82.368 s

    Connection rate: 1.2 conn/s (823.7 ms/conn <= 100 concurrent connections)
    Connection time [ms]: avg 82228.5 min 82209.1 max 82267.9 stddev 14.82
    Connect time [ms]: avg 0.1 min 0.0 max 0.7 stddev 0.12

    Request rate: 12140.7 req/s (0.1 ms/req)
    Request size [B]: avg 129.0 min 129.0 max 129.0 stddev 0.00

    Response rate: 12140.7 rsp/s (0.1 ms/rsp)
    Response size [B]: avg 8.0 min 8.0 max 8.0 stddev 0.00
    Response time [ms]: avg 8.2 min 0.7 max 35.7 stddev 0.00
    Response time [ms]: p25 7.0 p50 8.0 p75 9.0
    Response time [ms]: p95 12.0 p99 14.0 p999 20.0
    Response type: stored 1000000 not_stored 0 exists 0 not_found 0
    Response type: num 0 deleted 0 end 0 value 0
    Response type: error 0 client_error 0 server_error 0

    Errors: total 0 client-timo 0 socket-timo 0 connrefused 0 connreset 0
    Errors: fd-unavail 0 ftab-full 0 addrunavail 0 other 0

    CPU time [s]: user 28.67 system 45.68 (user 34.8% system 55.5% total 90.3%)
    Net I/O: bytes 130.7 MB rate 1624.3 KB/s (13.3*10^6 bps)

    $ printf "stats slabs\r\n" | nc -q 1 localhost 11211 | grep slab_curr| grep -v 'slab_curr 0'
    STAT 5:slab_curr 64

#### 1K requests, 1000 byte size, 100 conns, 100MB data

    $ ./mcperf --num-conns=100 --conn-rate=1000 --sizes=0.001 --num-calls=1000

    Total: connections 100 requests 100000 responses 100000 test-duration 8.239 s

    Connection rate: 12.1 conn/s (82.4 ms/conn <= 100 concurrent connections)
    Connection time [ms]: avg 8131.4 min 8122.4 max 8140.2 stddev 3.55
    Connect time [ms]: avg 0.1 min 0.0 max 1.9 stddev 0.27

    Request rate: 12137.0 req/s (0.1 ms/req)
    Request size [B]: avg 1030.0 min 1030.0 max 1030.0 stddev 0.00

    Response rate: 12137.0 rsp/s (0.1 ms/rsp)
    Response size [B]: avg 8.0 min 8.0 max 8.0 stddev 0.00
    Response time [ms]: avg 8.1 min 0.8 max 28.7 stddev 0.00
    Response time [ms]: p25 7.0 p50 8.0 p75 9.0
    Response time [ms]: p95 12.0 p99 14.0 p999 19.0
    Response type: stored 100000 not_stored 0 exists 0 not_found 0
    Response type: num 0 deleted 0 end 0 value 0
    Response type: error 0 client_error 0 server_error 0

    Errors: total 0 client-timo 0 socket-timo 0 connrefused 0 connreset 0
    Errors: fd-unavail 0 ftab-full 0 addrunavail 0 other 0

    CPU time [s]: user 2.88 system 5.02 (user 35.0% system 61.0% total 96.0%)
    Net I/O: bytes 99.0 MB rate 12303.0 KB/s (100.8*10^6 bps)

    $ printf "stats slabs\r\n" | nc -q 1 localhost 11211 | grep slab_curr| grep -v 'slab_curr 0'
    STAT 5:slab_curr 15
    STAT 13:slab_curr 49

#### 100 requests, 10000 byte size, 100 conns, 100MB data

    $ ./mcperf --num-conns=100 --conn-rate=1000 --sizes=0.0001 --num-calls=100

    Total: connections 100 requests 10000 responses 10000 test-duration 0.353 s

    Connection rate: 283.2 conn/s (3.5 ms/conn <= 100 concurrent connections)
    Connection time [ms]: avg 276.4 min 219.3 max 346.3 stddev 30.53
    Connect time [ms]: avg 0.2 min 0.0 max 3.8 stddev 0.51

    Request rate: 28320.1 req/s (0.0 ms/req)
    Request size [B]: avg 10031.0 min 10031.0 max 10031.0 stddev 0.00

    Response rate: 28320.1 rsp/s (0.0 ms/rsp)
    Response size [B]: avg 8.0 min 8.0 max 8.0 stddev 0.00
    Response time [ms]: avg 2.8 min 0.1 max 29.7 stddev 0.00
    Response time [ms]: p25 1.0 p50 1.0 p75 1.0
    Response time [ms]: p95 13.0 p99 18.0 p999 27.0
    Response type: stored 10000 not_stored 0 exists 0 not_found 0
    Response type: num 0 deleted 0 end 0 value 0
    Response type: error 0 client_error 0 server_error 0

    Errors: total 0 client-timo 0 socket-timo 0 connrefused 0 connreset 0
    Errors: fd-unavail 0 ftab-full 0 addrunavail 0 other 0

    CPU time [s]: user 0.03 system 0.17 (user 7.9% system 48.7% total 56.6%)
    Net I/O: bytes 95.7 MB rate 277642.1 KB/s (2274.4*10^6 bps)

    $ printf "stats slabs\r\n" | nc -q 1 localhost 11211 | grep slab_curr| grep -v 'slab_curr 0'
    STAT 5:slab_curr 6
    STAT 13:slab_curr 4
    STAT 23:slab_curr 54

#### 10 requests, 100000 byte size, 100 conns, 100MB data

    $ ./mcperf --num-conns=100 --conn-rate=1000 --sizes=0.00001 --num-calls=10

    Total: connections 100 requests 1000 responses 1000 test-duration 0.370 s

    Connection rate: 270.1 conn/s (3.7 ms/conn <= 100 concurrent connections)
    Connection time [ms]: avg 269.5 min 251.5 max 321.2 stddev 7.60
    Connect time [ms]: avg 0.1 min 0.0 max 0.3 stddev 0.08

    Request rate: 2701.3 req/s (0.4 ms/req)
    Request size [B]: avg 100032.0 min 100032.0 max 100032.0 stddev nan

    Response rate: 2701.3 rsp/s (0.4 ms/rsp)
    Response size [B]: avg 8.0 min 8.0 max 8.0 stddev 0.00
    Response time [ms]: avg 26.9 min 9.1 max 99.8 stddev 0.02
    Response time [ms]: p25 17.0 p50 20.0 p75 22.0
    Response time [ms]: p95 95.0 p99 99.0 p999 100.0
    Response type: stored 1000 not_stored 0 exists 0 not_found 0
    Response type: num 0 deleted 0 end 0 value 0
    Response type: error 0 client_error 0 server_error 0

    Errors: total 0 client-timo 0 socket-timo 0 connrefused 0 connreset 0
    Errors: fd-unavail 0 ftab-full 0 addrunavail 0 other 0

    CPU time [s]: user 0.08 system 0.17 (user 22.7% system 45.4% total 68.1%)
    Net I/O: bytes 95.4 MB rate 263903.7 KB/s (2161.9*10^6 bps)

    $ printf "stats slabs\r\n" | nc -q 1 localhost 11211 | grep slab_curr| grep -v 'slab_curr 0'
    STAT 5:slab_curr 2
    STAT 13:slab_curr 1
    STAT 23:slab_curr 10
    STAT 33:slab_curr 51

