import subprocess
from time import sleep

ARGS_UNARY = {
    'DAEMON':'-d',
    'MAX_CORE':'-r',
    'LOCK_PAGE':'-k',
    'HELP':'-h',
    'LICENSE':'-i',
    'LARGEPAGE':'-L',
    'PREALLOC':'-E',
    'CAS':'-C'
}

ARGS_BINARY = {
    'PORT':'-p',
    'UDP':'-U',
    'SOCKET':'-s',
    'ACCESS':'-a',
    'SERVER':'-l',
    'USER':'-u',
    'EVICTION':'-M',
    'MAX_MEMORY':'-m',
    'CONNECTIONS':'-c',
    'VERBOSITY':'-v',
    'PIDFILE':'-P',
    'FACTOR':'-f',
    'ITEM_MIN_SIZE':'-n',
    'DELIMITER':'-D',
    'THREADS':'-t',
    'BACKLOG':'-b',
    'SLAB_SIZE':'-I',
    'AGGR_INTERVAL':'-A',
    'SLAB_PROFILE':'-z'
}

EXEC = 'twemcache' # command to launch twemcache
INIT_DELAY = 0.3 # time before twemcache is ready to serve traffic
SHUTDOWN_DELAY = 0.3 # time before twemcache is completely shut down

# Machine/Build dependent arguments- sizes
tc = subprocess.Popen([EXEC, '-S'], stderr=subprocess.PIPE)
output = tc.stderr.readlines()[2:]
args = {}
for line in output:
  key,val = line.split()
  args[key] = val
NATIVE_ITEM_OVERHEAD = int(args['item_hdr_size'])
NATIVE_ITEM_SIZE = int(args['item_chunk_size'])
NATIVE_SLAB_OVERHEAD = int(args['slab_hdr_size'])
NATIVE_SLAB_SIZE = int(args['slab_size'])


# Unary arguments
DAEMON = False
MAX_CORE = False
LOCK_PAGE = False
HELP = False
LICENSE = False
LARGEPAGE = False
PREALLOC = False
CAS = False

# Binary arguments
PORT = '11211' # server (TCP) port (-p)
UDP = None
SOCKET = None
ACCESS = None
SERVER = '127.0.0.1' # server IP (-l)
USER = None
EVICTION = 2 # random slab eviction by default
MAX_MEMORY = 64 # max amount memory allocated, in MB (-m)
CONNECTIONS = 256 # max number of concurrent connections allowed (-c)
VERBOSITY = 5 # logging verbosity
PIDFILE = None
FACTOR = 1.25 # factor of slab growth (-f)
ITEM_MIN_SIZE = NATIVE_ITEM_SIZE # item size, smallest (-n)
DELIMITER = None
THREADS = 2 # number of threads (-t)
BACKLOG = 1024
SLAB_SIZE = NATIVE_SLAB_SIZE # (-I)
AGGR_INTERVAL = 100000 # aggregation interval of stats, in milliseconds (-A)
SLAB_PROFILE = None # (-z)

# internals, not used by launching service but useful for data generation
ALIGNMENT = 8 # bytes
SLAB_OVERHEAD = NATIVE_SLAB_OVERHEAD # per-slab storage overhead at the server
ITEM_OVERHEAD = NATIVE_ITEM_OVERHEAD # per-item storage overhead at the server
CAS_LEN = 8 # cas adds another 8 bytes

# global stats (returns of "stats" command)
STATS_KEYS = [ # system/service info
    'pid', 'uptime', 'time', 'version', 'pointer_size', 'aggregate_ts',
    'rusage_user', 'rusage_system',
     # connection related
    'conn_disabled', 'conn_total', 'conn_struct', 'conn_yield', 'conn_curr',
     # item/slab related
    'item_curr', 'item_free', 'item_acquire', 'item_remove', 'item_evict', 'item_expire',
    'slab_req', 'slab_error', 'slab_alloc', 'slab_curr', 'slab_evict',
     # things in bytes
    'data_read', 'data_written', 'data_curr', 'data_value_curr',
     # time stamps
    'item_expire_ts', 'item_retire_ts', 'item_evict_ts',
    'slab_req_ts', 'slab_error_ts', 'slab_alloc_ts', 'slab_new_ts', 'slab_evict_ts',
     # command related
    'set', 'set_success',
    'add', 'add_exist', 'add_success',
    'replace', 'replace_hit', 'replace_miss', 'replace_success',
    'append', 'append_hit', 'append_miss', 'append_success',
    'prepend', 'prepend_hit', 'prepend_miss', 'prepend_success',
    'delete', 'delete_hit', 'delete_miss',
    'incr', 'incr_hit', 'incr_miss', 'incr_success',
    'decr', 'decr_hit', 'decr_miss', 'decr_success',
    'cas', 'cas_badval', 'cas_hit', 'cas_miss', 'cas_success',
    'get', 'get_hit', 'get_miss',
    'gets', 'gets_hit', 'gets_miss',
    'flush',
    'stats',
     # general errors
    'cmd_error', 'server_error',
    'klog_logged', 'klog_discarded', 'klog_skipped'
    ]
SETTINGS_KEYS = [
    'prealloc', 'lock_page', 'accepting_conns', 'daemonize', 'max_corefile',
    'cas_enabled', 'num_workers', 'reqs_per_event', 'oldest',
    'log_filename', 'verbosity', 'maxconns', 'tcpport', 'udpport', 'inter',
    'domain_socket', 'umask', 'tcp_backlog', 'evictions', 'growth_factor', 'maxbytes',
    'chunk_size', 'slab_size', 'username', 'stats_agg_intvl']
STATS_DELAY = float(AGGR_INTERVAL) * 1.5 / 1000000
# time we wait before getting for stats
