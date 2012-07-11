## Overview
twctop is a tool that presents realtime cache stats in a intuitive way. Different 'views' of the same stats help users gaining insight into the cluster, which is particularly useful for debugging. When multiple hosts are included, stats are aggregated in a way that suits the current view. It is written in Ruby and consumes a YAML file with a particular format for historical reasons at Twitter.

## How to use twctop.rb

To monitor a cluster:

    $ twctop.rb [options] </path/to/yaml>

or to monitor a single instance:

    $ twctop.rb [options] <-H hostname:port>

## Most Useful Options

* -s, --sleep <num-of-sec>: 'watch'-style endless refreshing. After the initial stats query, ongoing rates since the last stats query are shown when applicable, rather than the absolutes since server start. A reasonable value for most pools in a low latency environment is 2.
* -v, --view [slab|host|command]: Choose from one of the three views to start. Content shown in each view is explained below. After launch, one can still switch views by pressing the initial letter of each view.
* -h, --help: full help message

## The YAML format
The YAML file should look like the following, with the list servers replaced by the ones to be monitored:

<pre>
production:
  servers:
    - 1.2.3.4:11111:20
    - 1.2.3.5:11111:20
</pre>

## The SLAB View

The header on top shows slabs allocated, how many "slots" are used to store items, and how many the allocated slabs can host.

The meaning of the columns in this view are listed below:
* SLAB: Slab class, marked with the size of a single item within this slab class.
* #SLAB: Number of slabs allocated into a slab class.
* ITEM(%): What percentage of allocated slots are occupied.
* KEYVAL(%): Data (key+value, no metadata) size compared to the memory size holding them.
* VALUE(%): Size of values compared to that of key-value pairs, in percentage.
* BAR: A figurative way of showing both how much memory has been used (i.e. cannot be used by other items) and how much has been allocated. It is also the easiest way to spot memory distribution among slabs. Bar widths are normalized.
* REC(/s): Rate at which expired items are reclaimed (recycled) to be used by new items.
* EVT(/s): Rate at which unexpired items are evicted (recycled) to be used by new items.
* SEVT(/s): Rate at which slabs are (randomly) evicted to be used by new items (of any slab class).
* LOCATE(/s): Rate at which existing items are found, this happens to both read, update commands and delete.
* INSERT(/s): Rate at which new items are inserted, this happends to all write commands.

## The HOST View

The header on top shows how much data are stored out of the total capacity. (Note: data include both key, value and item related information, but not unused slots or item metadata.)

The meaning of the columns in this view are listed below:
* INSTANCE: IP and port number of the memcached instance.
* UTIL(%): Data size compared to maximum memory allowed, in percentage.
* CONN: Number of open connections to the host.
* LATENCY: Time to execute a "stats" command, including the time to read the entirety of response.
* EXPIRE(/s): Rate at which expired items are reclaimed by the memcached instance.
* EVICT(/s): Rate at which unexpired items are evicted to make room for new items.
* BYTE_IN/s: Number of bytes received by an instance (TCP/UDP payload only).
* BYTE_OUT/s: Number of bytes sent by an instance (TCP/UDP payload only).
* R_REQ(/s): Rate at which read commands arrive.
* W_REQ(/s): Rate at which write commands arrive.
* D_REQ(/s): Rate at which delete commands arrive.
* SVRERR(/s): Rate at which a server instance is throwing errors (OOM, for example).

## The COMMAND View

The header on top shows how many requests are correctly processed, compared to the total count.

The meaning of the columns in this view are listed below:
* REQUEST(/s): Rate at which commands are sent.
* MIX(%): Percentage in all requests.
* SUCCESS(%): Percentage of commands returning success, for write commands only.
* HIT(%): Percentage of requests hitting an existing item, for read, update and delete.
* MISS(%): Percentage of requests hitting an existing item, for read, update and delete.
* EXISTS(%): Percentage of requests failing because of existing items, for cas and add only.
* ERROR(/s): Rate at which command errors (syntax error or other unknown problems).

