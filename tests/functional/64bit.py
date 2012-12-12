__doc__ = '''
Testing twemcache with large memory on 64-bit OS.
'''
__author__ = "Yao Yue <yao@twitter.com>"
__version__ = "0.1-1.45"

import os
import sys
import subprocess
try:
    from lib import memcache
except ImportError:
    print "Check your sys.path setting to include lib/memcache.py."
    sys.exit()
try:
    import unittest2 as unittest
except ImportError:
    print "older version of unittest may not handle skipTest properly"
    import unittest

# handling server and data configurations
from config.defaults import *
from lib.utilities import *
from lib.logging import print_module_title, print_module_done

memAlloc = 4098

class Args(object):
    def __init__(self, command=None, config=None):
        self.command = command
        self.config = config

def setUpModule():
    print_module_title(__name__)
    global counter
    counter = 0

def tearDownModule():
    print_module_done(__name__)


class Functional64bit(unittest.TestCase):

    # setup&teardown client
    def setUp(self):
        global counter
        counter += 1
        print "  running test %d" % counter
        self.server = None
        self.mc = memcache.Client(["%s:%s" % (SERVER, PORT)], debug=0)

    def tearDown(self):
        if self.server:
            stopServer(self.server)

    #
    # tests
    #
    def test_64bit(self):
        '''64bit specific test.'''
        # check memory to see how much free memory we have
        # otherwise the test will fail if we run out of free memory
        memstr = subprocess.Popen(['cat','/proc/meminfo'],
                                  stdout=subprocess.PIPE).communicate()[0]
        meminfo = {}
        for line in memstr.split('\n'):
            if line:
                name, value = line.split(':')
                meminfo[name] = value.strip()
        global memFree
        memFree = int(meminfo['MemFree'].rstrip(' kB')) / 1024
        # use PREALLOC to test heap size
        args = Args(command='MAX_MEMORY = %d\nEVICTION = 0\nPREALLOC=True' % memAlloc)
        self.server = startServer(args)
        self.assertIsNotNone(self.server)
        build = self.mc.get_stats()[0][1]['pointer_size']
        if build == '32':
            self.skipTest("skipping 64-bit test on a 32-bit twemcache build")
        if memFree < memAlloc:
            self.skipTest("not enough memory to for the %d MiB intended" % memAlloc)
        self.assertEqual('64', build)
        statsettings = self.mc.get_stats('settings')
        self.assertEqual(str(memAlloc*1024*1024), statsettings[0][1]['maxbytes'])
        active = 0
        statslabs = self.mc.get_slabs()[0][1]
        for slab in statslabs:
            if int(statslabs[slab]['slab_curr']) > 0:
                active += 1
        self.assertEqual(0, active)
        for key in range(0, 10):
            size = int(statsettings[0][1]['slab_size']) - ITEM_OVERHEAD - SLAB_OVERHEAD\
                   - CAS_LEN - len(str(key) + '\0') - 2 # CRLF_LEN
            self.mc.set(str(key), 'a' * size)
            self.assertIsNotNone(self.mc.get(str(key)))
            key += 1
        self.assertIsNone(self.mc.get(str(key)))
        active = 0
        statslabs = self.mc.get_slabs()[0][1]
        for slab in statslabs:
            if int(statslabs[slab]['slab_curr']) > 0:
                active += 1
        self.assertEqual(1, active)


if __name__ == '__main__':
    functional_64bit = unittest.TestLoader().loadTestsFromTestCase(Functional64bit)
    unittest.TextTestRunner(verbosity=2).run(functional_64bit)
