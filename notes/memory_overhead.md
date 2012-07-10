## Question

What is the memory overhead of twemcache when it configured to use 'X' GB of memory for all items (i.e. the -m option to twemcache is X / 1024)

## Tl; Dr

For hash module, a 4G twemcache consumes 256M for hash table; a 8G would consume 512M and a 16G would consume 1GB!
For connection module, 10K active connections we are consuming ~256M, for 50K, we will be consuming ~1.4G and for 100K we will be consuming ~2.6G

## Details

In twemcache memory is consumed by the following modules:
+ Thread module
+ Stats module
+ Klogger module
+ Slabs module
+ Hash module
+ Connection module

### Thread Module

Memory consumed by the thread module is negligible.

### Stats Module

Memory consumed by the stats module can be computed as follows. The number of worker threads (specified by -t option) is by default set to 4. Besides worker threads, we have the dispatcher, aggregator and keylogger thread. So, when invoked with default options there are 7 threads. Each of these threads collect thread local stats. Each of these thread local stats do the following memory allocation:

+ sizeof(stats_tmetric) = 1504
+ stats_thread = alloc(sizeof(stats_tmetrics)); = 1504 = 1.5K
+ stats_slabs = alloc(MAX_NUM_OF_SLABCLASSES * sizeof(struct stats_metric *)) = 255 * 8 = 2040 = 2K
+ sizeof(stats_smetrics) = 1248
+ metric_array2 = mc_alloc(MAX_NUM_OF_SLABCLASSES * sizeof(stats_smetrics)) =  255 * 128 = 318240 = 318K

So, in total ~2M consumed for setup of every thread, which is negligible.  Also, for every connection we have a stats buffer, that grows depending on the # of stats that we need to send out:

    if (nsize != c->stats.size) {
        char *ptr = mc_realloc(c->stats.buffer, nsize);
        if (ptr) {
            c->stats.buffer = ptr;
            c->stats.size = nsize;

With the current # stats, this value grows to 64K. But this is freed immediately by core_write_and_free() after the stats are sent out. So again, memory consumed by stats module is negligible

### Slabs Module

Besides the memory value given by -m option, slab module consumes memory for the slabtable, which has one 8 byte entry for every slab in the system. So, for 4G twemcache (-m 4096), we have:

+ slab_info = {base = 0x0, curr = 0x0, nr_slabs = 4096, max_slabs = 4096, slabtable = 0xce8cd0}
+ slabtable = 8 * 4096 = 32K

So, for X GB of twemcache, slabtable will take up, X * 1024 * 2^3 bytes, which is still in range of few low MBs and hence negligible.

### Hash Module

Hash table initially starts of with 2^16 buckets and grows an number of buckets every time by a factor of 2 when the # hash items is greater that 1.5 times the number of hash buckets

    #define HASHSIZE(_n) (1UL << (_n))
    static uint32_t hashpower = 16;

    hash_items++;
    if (!expanding && (hash_items > (HASHSIZE(hashpower) * 3) / 2)) {
        _assoc_expand();
    }

    static void
    _assoc_expand(void)
    {
        uint32_t hashtable_sz = HASHSIZE(hashpower + 1);

        old_hashtable = primary_hashtable;
        primary_hashtable = _assoc_create_table(hashtable_sz);
        if (primary_hashtable != NULL) {
            log_debug(LOG_DEBUG, "expanding hash table from %u to %u buckets",
                      HASHSIZE(hashpower), hashtable_sz);
            hashpower++;

So, initially the hash table consumes 2^16 * 2^3 bytes = 512K bytes, and over time as more and more items gets added to the hash table, the hash table expands by a factor of 2. Lets calculate the maximum hash table size for a 4G twemcache (-m 4096); +For a 4G twemcache with 128 byte smallest item size+, the maximum # hash items are:

+ 4096 * (1024 * 1024 / 128) = 33554432.00
+ log_2(33554432.00) = 25.10
+ 2^25 * 1.5 = 50331648.0
+ 50331648.0 > 33554432.00 = 1

So, the hash table size is  = 2^25 * 2^3 = 256M. So, a 4G twemcache consumes 256M for hash table; a 8G would consume 512M and a 16G would consume 1GB!

Lets calculate the maximum hash table size for a 16G twemcache (-m 65536)

+ 16 * 1024 * (1024 * 1024 / 128) = 134217728.00
+ log_2(134217728.00) = 27.11
+ 2^27 * 1.5 - 201326592.0
+ 134217728.00 < 201326592.0 =- 1

So, for any twemcache instance with "X" GB of slab space, the maximum hash table size is:

+ max_slabs = X * 1024
+ min_item_size = 128
+ max_items_per_slab = (1024 * 1024 / min_item_size)
+ max_items = max_slabs * max_items_per_slab
+ hash_buckets = floor(log_2(max_items))
+ hash_size is between 2^hash_buckets * 2^3 and 2^(hash_buckets + 1) * 2^3

### Connection Module

Every connection objects allocates:

+ c = mc_zalloc(sizeof(struct conn)) = 496 bytes
+ c->rsize = read_buffer_size;
+ c->rbuf = mc_alloc((size_t)c->rsize) = 2048
+ c->wsize = DATA_BUFFER_SIZE;
+ c->wbuf = mc_alloc((size_t)c->wsize) = 2048
+ c->isize = ITEM_LIST_INITIAL;
+ c->ilist = mc_alloc(sizeof(struct item *) * c->isize) = 200 * 8 = 1600
+ c->suffixsize = SUFFIX_LIST_INITIAL;
+ c->suffixlist = mc_alloc(sizeof(char *) * c->suffixsize) = 20 * 8 = 160 bytes
+ c->iovsize = IOV_LIST_INITIAL;
+ c->iov = mc_alloc(sizeof(struct iovec) * c->iovsize) = 400 * 16 = 6400
+ c->msgsize = MSG_LIST_INITIAL;
+ c->msglist = mc_alloc(sizeof(struct msghdr) * c->msgsize) = 10 * 56 = 560 bytes

Total initial bytes per conn = 13152 = ~13K. So, even if 50K connections are active, memory used will be = 13K * 10K = 130M.  But because of the realloc strategy, each rbuf is 16K (in virtual memory), which it then shrinks to 2K in conn_shrink. This means that we have an additional 14K addition per conn, leading to around 27K

    new_rbuf = mc_realloc(c->rbuf, c->rsize * 2);
    if (new_rbuf == NULL) {
        stats_thread_incr(server_error);
        c->rbytes = 0; /* ignore what we read */
        out_string(c, "SERVER_ERROR out of memory reading request");
        c->write_and_go = CONN_CLOSING;
        return READ_MEMORY_ERROR;
    }
    c->rcurr = c->rbuf = new_rbuf;
    c->rsize *= 2;

So, for 10K active connections we are consuming 270M, for 50M, we will be consuming 1350M and for 100K we will be consuming 2700M

## Conclusion

The main contributors to memory overhead in twemcache are:

+ Connection module
+ Hash module

For hash module, a 4G twemcache consumes 256M for hash table; a 8G would consume 512M and a 16G would consume 1GB!
For connection module, 10K active connections we are consuming ~256M, for 50K, we will be consuming ~1.4G and for 100K we will be consuming ~2.6G

