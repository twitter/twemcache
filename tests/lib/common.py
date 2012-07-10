__doc__ = '''
Client classes for load testing.
'''
__author__ = "Yao Yue <yao@twitter.com>"
__version__ = "0.1-1.45"

import sys
import time
import random
try:
    from lib import memcache
except ImportError:
    print "Place memcache module (version 1.45) in lib."
    sys.exit()

from config.defaults import *

def _randstr(length):
    '''This random string generator is extremely naive. Luckily we don't care.'''
    bits = random.getrandbits(length * 4)
    return "%0x" % bits

def _set_single_stored(mc, item):
    '''
    operation: set
    item access mode: single (specified)
    data source: stored
    '''
    key, val = item
    mc.set(key, val)

def _set_single_unbounded(mc, bucket):
    '''
    operation: set
    bucket: single (specified)
    item access mode: random
    data source: randomly generated, thus unbounded in unique entries
    '''
    key = _randstr(KEY_SIZE)
    val = _randstr(bucket)
    mc.set(key, val)

def _get_single(mc, key=None):
    '''
    operation: get
    key source: specified or randomly generated
    '''
    if (key == None): # create a key that's most likely not cached
        key = _randstr(KEY_SIZE)
    mc.get(key)

class Client:
    '''
    Each member provides a unique pattern to submit client-side requests.
    '''
    def __init__(self,
                 data,
                 server=SERVER, port=PORT,
                 freq=FREQUENCY):
        self.freq = freq
        self.data = data
        random.seed()
        self.mc = memcache.Client(['%s:%s' % (server, port)], debug=0)

#
# setters
#
    def setter_filler(self, bucket=None):
        '''
        warm-up/fill twemcache server with set commands.
        if bucket is not specified, all buckets are filled with pregenerated data.
        '''
        if bucket == None:
            buckets = self.data.buckets
        else:
            buckets = [bucket]
        for bucket in buckets:
            for offset in range(0, self.data.rows[bucket].width):
                _set_single_stored(self.mc, self.data.rows[bucket].items[offset])

    def setter_fixed_single(self, bucket=None, offset=0, duration=-1.0):
        '''
        repeatedly set a specified item belonging to a fixed bucket
        items are pulled from pre-generated data store
        duration: in seconds, negative value means infinite loop
        '''
        if (bucket == None):
            bucket = self.data.buckets[random.randrange(len(self.data.buckets))]
        epoch = 1.0 / self.freq  # one action every epoch, this is coarse-grain
        tries = 1 + int(duration * self.freq)
        while (tries != 0):
            _set_single_stored(self.mc, self.data.rows[bucket].items[offset])
            tries -= 1
            time.sleep(epoch)

    def setter_fixed_random(self, bucket=None, duration=-1.0):
        '''
        repeatedly set an item belonging to a fixed bucket
        items are pulled from pre-generated data store
        duration: in seconds, negative value means infinite loop
        '''
        if (bucket == None):
            bucket = self.data.buckets[random.randrange(len(self.data.buckets))]
        epoch = 1.0 / self.freq  # one action every epoch, this is coarse-grain
        tries = 1 + int(duration * self.freq)
        while (tries != 0):
            offset = random.randrange(self.data.rows[bucket].width)
            _set_single_stored(self.mc, self.data.rows[bucket].items[offset])
            tries -= 1
            time.sleep(epoch)

    def setter_fixed_unbounded(self, bucket=None, duration=-1.0):
        '''
        repeatedly set an item belonging to a fixed bucket
        items are randomly generated, meaning zero repetition
        duration: in seconds, negative value means infinite loop
        '''
        if (bucket == None):
            bucket = self.data.buckets[random.randrange(len(self.data.buckets))]
        epoch = 1.0 / self.freq  # one action every epoch, this is coarse-grain
        tries = 1 + int(duration * self.freq)
        while (tries != 0):
            _set_single_unbounded(self.mc, bucket)
            tries -= 1
            time.sleep(epoch)

    def setter_random_random(self, duration=-1.0):
        '''
        repeatedly set an item belonging to a random bucket
        items are pulled from pre-generated data store
        duration: in seconds, negative value means infinite loop
        '''
        epoch = 1.0 / self.freq  # one action every epoch, this is coarse-grain
        tries = 1 + int(duration * self.freq)
        while (tries != 0):
            bucket = self.data.buckets[random.randrange(len(self.data.buckets))]
            offset = random.randrange(self.data.rows[bucket].width)
            _set_single_stored(self.mc, self.data.rows[bucket].items[offset])
            tries -= 1
            time.sleep(epoch)

    def setter_random_unbounded(self, duration=-1.0):
        '''
        repeatedly set an item belonging to a random bucket
        items are randomly generated, meaning zero repetition
        duration: in seconds, negative value means infinite loop
        '''
        epoch = 1.0 / self.freq
        tries = 1 + int(duration * self.freq)
        while (tries != 0):
            bucket = self.data.buckets[random.randrange(len(self.data.buckets))]
            _set_single_unbounded(self.mc, bucket)
            tries -= 1
            time.sleep(epoch)


#
# appenders/prepender
#

    def appender_fixed_random(self, bucket=None, length=256, duration=-1.0):
        '''
        repeatedly append to items 'originally' belonging to a fixed bucket,
        thus effectively, over a period of time, promoting them toward
        larger slab classes.
        Doing this for too long or appending too much too fast will cause
        items to grow beyond the maximum acceptable length, thus
        causing an error at server (which hopefully is gracefully handled).
        '''
        if (bucket == None):
            bucket = self.data.buckets[random.randrange(len(self.data.buckets))]
        epoch = 1.0 / self.freq
        tries = 1 + int(duration * self.freq)
        while (tries != 0):
            offset = random.randrange(self.data.rows[bucket].width)
            key = self.data.rows[bucket].items[offset][0]
            suffix = _randstr(length)
            self.mc.append(key, suffix)
            tries -= 1
            time.sleep(epoch)

    def appender_random_random(self, length=256, duration=-1.0):
        '''
        repeatedly append to items 'originally' belonging to a random bucket,
        thus effectively, over a period of time, promoting all items toward
        larger slab classes.
        Doing this for too long or appending too much too fast will cause
        items to grow beyond the maximum acceptable length, thus
        causing an error at server (which hopefully is gracefully handled).
        '''
        epoch = 1.0 / self.freq
        tries = 1 + int(duration * self.freq)
        while (tries != 0):
            bucket = self.data.buckets[random.randrange(len(self.data.buckets))]
            offset = random.randrange(self.data.rows[bucket].width)
            key = self.data.rows[bucket].items[offset][0]
            suffix = _randstr(length)
            self.mc.append(key, suffix)
            tries -= 1
            time.sleep(epoch)

    def prepender_fixed_random(self, bucket=None, length=256, duration=-1.0):
        '''
        repeatedly append to items 'originally' belonging to a fixed bucket,
        thus effectively, over a period of time, promoting them toward
        larger slab classes.
        Doing this for too long or appending too much too fast will cause
        items to grow beyond the maximum acceptable length, thus
        causing an error at server (which hopefully is gracefully handled).
        '''
        if (bucket == None):
            bucket = self.data.buckets[random.randrange(len(self.data.buckets))]
        epoch = 1.0 / self.freq
        tries = 1 + int(duration * self.freq)
        while (tries != 0):
            offset = random.randrange(self.data.rows[bucket].width)
            key = self.data.rows[bucket].items[offset][0]
            prefix = _randstr(length)
            self.mc.prepend(key, prefix)
            tries -= 1
            time.sleep(epoch)

    def prepender_random_random(self, length=256, duration=-1.0):
        '''
        repeatedly append to items 'originally' belonging to a random bucket,
        thus effectively, over a period of time, promoting all items toward
        larger slab classes.
        Doing this for too long or appending too much too fast will cause
        items to grow beyond the maximum acceptable length, thus
        causing an error at server (which hopefully is gracefully handled).
        '''
        epoch = 1.0 / self.freq
        tries = 1 + int(duration * self.freq)
        while (tries != 0):
            bucket = self.data.buckets[random.randrange(len(self.data.buckets))]
            offset = random.randrange(self.data.rows[bucket].width)
            key = self.data.rows[bucket].items[offset][0]
            prefix = _randstr(length)
            self.mc.prepend(key, prefix)
            tries -= 1
            time.sleep(epoch)


#
# getters
#
    def getter_fixed_single(self, bucket=None, offset=0, duration=-1.0):
        '''
        repeatedly get a specified item belonging to a fixed bucket
        keys are pulled from pre-generated data store
        duration: in seconds, negative value means infinite loop
        '''
        if (bucket == None):
            bucket = self.data.buckets[random.randrange(len(self.data.buckets))]
        epoch = 1.0 / self.freq  # one action every epoch, this is coarse-grain
        tries = 1 + int(duration * self.freq)
        while (tries != 0):
            _get_single(self.mc, self.data.rows[bucket].items[offset][0])
            tries -= 1
            time.sleep(epoch)

    def getter_fixed_random(self, bucket=None, duration=-1.0):
        '''
        repeatedly get a random item belonging to a fixed bucket
        keys are pulled from pre-generated data store
        duration: in seconds, negative value means infinite loop
        '''
        if (bucket == None):
            bucket = self.data.buckets[random.randrange(len(self.data.buckets))]
        epoch = 1.0 / self.freq  # one action every epoch, this is coarse-grain
        tries = 1 + int(duration * self.freq)
        while (tries != 0):
            offset = random.randrange(self.data.rows[bucket].width)
            _get_single(self.mc, self.data.rows[bucket].items[offset][0])
            tries -= 1
            time.sleep(epoch)

    def getter_random_random(self, duration=-1.0):
        '''
        repeatedly get a random item belonging to a random bucket
        keys are pulled from pre-generated data store
        duration: in seconds, negative value means infinite loop
        '''
        epoch = 1.0 / self.freq  # one action every epoch, this is coarse-grain
        tries = 1 + int(duration * self.freq)
        while (tries != 0):
            bucket = self.data.buckets[random.randrange(len(self.data.buckets))]
            offset = random.randrange(self.data.rows[bucket].width)
            _get_single(self.mc, self.data.rows[bucket].items[offset][0])
            tries -= 1
            time.sleep(epoch)

    def getter_unbounded(self, duration=-1.0):
        '''
        repeatedly getting an item that's most likely not in the cache.
        '''
        epoch = 1.0 / self.freq  # one action every epoch, this is coarse-grain
        tries = 1 + int(duration * self.freq)
        while (tries != 0):
            _get_single(self.mc)
            tries -= 1
            time.sleep(epoch)



class Monitor():
    ''' A monitor is a twemcache client that collects server stats periodically'''
    def __init__(self,
                 server=SERVER, port=PORT,
                 logfile=STATS_LOG,
                 freq=STATS_FREQ):
        self.log = open(logfile, 'w')
        self.freq = freq
        self.mc = memcache.Client(['%s:%s' % (server, port)], debug=0)


    def run(self, duration=-1.0):
        epoch = 1.0 / self.freq
        tries = 1 + int(duration * self.freq)
        while (tries != 0):
            stats = self.mc.get_stats()
            self.log.write("%s\n" % time.time())
            for entry in stats:
                name, data = entry
                self.log.write("%s\n" % name)
                for key in data.keys():
                    self.log.write("%s: %s\n" % (key, data[key]))
                self.log.write("\n")
            self.log.flush()
            tries -= 1
            time.sleep(epoch)


class Row():
    '''
    Each row stores data for a bucket.
    '''
    def __init__(self, width, items):
        self.width = width
        self.items = items

class Data():
    '''
    Each member provides a unique layout of data to generate traffic from.
    The layout framework follows that of a twemcache server's slab/item structure.
    '''
    def __init__(self, configfile):
        '''
            Generate the items beforehand;
            return them with metadata.
        '''
        f = open(configfile)
        lines = f.readlines()
        self.rows = {}
        for line in lines:
            bucket, width = map(lambda x: int(x), line.split())
            items = []
            for i in xrange(width):
                key = _randstr(KEY_SIZE)
                val = _randstr(bucket - ITEM_OVERHEAD - KEY_SIZE - 10)
                items.append((key,val))
            self.rows[bucket] = Row(width, items)
        self.buckets = self.rows.keys()
