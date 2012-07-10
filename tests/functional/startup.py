__doc__ = '''
Testing the various startup parameters.
'''
__author__ = "Yao Yue <yao@twitter.com>"
__version__ = "0.1-1.45"

import os
import sys
import time
import subprocess
try:
    from lib import memcache
except ImportError:
    print "Check your sys.path setting to include lib/memcache.py."
    sys.exit()
try:
    import unittest2 as unittest
except ImportError:
    import unittest

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


class FunctionalStartup(unittest.TestCase):

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
    def test_default(self):
        '''use default settings.'''
        config = os.path.expanduser(TESTS_PATH) + '/config/server/default.py'
        execfile(config)
        args = Args(config=config)
        self.server = startServer(args)
        self.assertIsNotNone(self.server)
        stats = self.mc.get_stats('settings')
        values = stats[0][1]
        self.assertEqual(str(THREADS), values['num_workers'])
        self.assertEqual(str(PORT), values['tcpport'])
        self.assertEqual(str(SERVER), values['interface'])
        self.assertEqual(str(MAX_MEMORY * 1024 * 1024), values['maxbytes'])
        self.assertEqual(str(CONNECTIONS), values['maxconns'])
        self.assertEqual(str(ITEM_MIN_SIZE), values['chunk_size'])
        self.assertEqual(str(BACKLOG), values['tcp_backlog'])
        self.assertEqual(str(SLAB_SIZE), values['slab_size'])
        self.assertEqual(str(FACTOR), values['growth_factor'])


    def test_cas(self):
        '''disable cas, -C'''
        args = Args(command='CAS = True')
        self.server = startServer(args)
        self.assertIsNotNone(self.server)
        stats = self.mc.get_stats('settings')
        self.assertEqual('0', stats[0][1]['cas_enabled'])

    def test_prealloc(self):
        '''prealloc, -E'''
        args = Args(command='PREALLOC = True\nMAX_MEMORY = 2\nEVICTION = 1')
        self.server = startServer(args)
        self.assertIsNotNone(self.server)
        stats =self.mc.get_stats('settings')
        self.assertEqual('1', stats[0][1]['prealloc'])
        self.assertEqual(True, self.mc.set("foo", "bar"))
        self.assertEqual(True, self.mc.set("foo" * 10, "bar" * 10))
        self.assertEqual(False, self.mc.set("foo" * 50, "bar" * 50))

    def test_daemon(self):
        '''daemon, -d'''
        args = Args(command='DAEMON = True\nPIDFILE = "/tmp/mcpid"')
        self.server = startServer(args)
        self.assertIsNotNone(self.server)
        pid = open("/tmp/mcpid").read().rstrip()
        stats =self.mc.get_stats()
        self.assertEqual(pid, stats[0][1]['pid'])
        subprocess.Popen(['kill', pid])
        time.sleep(SHUTDOWN_DELAY)
        self.assertEqual(False, self.mc.set("foo", "bar"))# cannot connect

    def test_maxconn(self):
        '''maximum connections, -c'''
        args = Args(command='CONNECTIONS = 10')
        self.server = startServer(args)
        self.assertIsNotNone(self.server)
        conns = {}
        for i in range(0, 10):
            conns[i] = memcache.Client(["%s:%s" % (SERVER, PORT)])
            self.assertTrue(conns[i].set("%d" % i, "%d" % i))

    def test_aggrintrvl(self):
        '''aggregation interval, -A'''
        args = Args(command='AGGR_INTERVAL = 1000000')
        self.server = startServer(args)
        self.assertIsNotNone(self.server)
        stats = self.mc.get_stats('settings')
        self.assertEqual(1.0, float(stats[0][1]['stats_agg_intvl']))

    def test_slabsize(self):
        '''Choose a different slab/max-item size, -I'''
        # jumbo slabs
        args = Args(command='SLAB_SIZE = 1024 * 1024 * 4')
        slabsize = 1024 * 1024 * 4
        self.server = startServer(args)
        self.assertIsNotNone(self.server)
        # here we use a different memcache client to set value length differently
        mc = memcache.Client(["%s:%s" % (SERVER, PORT)], debug=0, server_max_value_length=slabsize)
        mc.set("lval", 'a' * (slabsize - 512))
        self.assertEqual(slabsize - 512, len(mc.get("lval")))

    def test_slabfile(self):
        '''Initalize slab classes with a size profile, -z'''
        # create a slab profile first
        args = Args(command='SLAB_PROFILE = "64,128,256"')
        self.server = startServer(args)
        self.assertIsNotNone(self.server)


if __name__ == '__main__':
    functional_startup = unittest.TestLoader().loadTestsFromTestCase(FunctionalStartup)
    unittest.TextTestRunner(verbosity=2).run(functional_startup)
