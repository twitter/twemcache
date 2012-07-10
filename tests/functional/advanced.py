__doc__ = '''
Testing not-so-basic functionalities,
such as eviction, lru, read/write large data from socket...
'''
__author__ = "Yao Yue <yao@twitter.com>"
__version__ = "0.1-1.45"

import sys
import time
try:
    from lib import memcache
except ImportError:
    print "Check your sys.path setting to include lib/memcache.py."
    sys.exit()
try:
    import unittest2 as unittest
except ImportError:
    import unittest
from multiprocessing import Process, Pipe

# handling server and data configurations
from config.defaults import *
from lib.utilities import *
from lib.logging import print_module_title, print_module_done

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


class FunctionalAdvanced(unittest.TestCase):

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

    def test_lru(self):
        ''' test lru algorithm '''
        args = Args(command='MAX_MEMORY = 8\nEVICTION = 1\nTHREADS = 1') #lru eviction
        size = SLAB_SIZE - ITEM_OVERHEAD - SLAB_OVERHEAD - 32 # ITEM_OVERHEAD
        data = '0' * size
        self.server = startServer(args)
        self.assertTrue(self.mc.set("big0", data))
        self.assertEqual("0", self.mc.get_stats()[0][1]['item_evict'])
        for i in range(1, 10):
            self.assertTrue(self.mc.set("big%d" % i, str(i)*size))
        time.sleep(STATS_DELAY)
        evictions = int(self.mc.get_stats()[0][1]['item_evict'])
        self.assertTrue(evictions >= 2 and evictions < 10)
        for i in range(0, evictions):
            self.assertIsNone(self.mc.get("big%d" % i))
        for i in range(evictions, 10):
            self.assertEqual(str(i) * size, self.mc.get("big%d" % i))


if __name__ == '__main__':
    functional_advanced = unittest.TestLoader().loadTestsFromTestCase(FunctionalAdvanced)
    unittest.TextTestRunner(verbosity=2).run(functional_advanced)
