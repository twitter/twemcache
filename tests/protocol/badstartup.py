__doc__ = '''
Testing when various startup parameters go wild.
'''
__author__ = "Yao Yue <yao@twitter.com>"
__version__ = "0.1-1.45"

import os
import time
import sys
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
    global server
    server = startServer()

def tearDownModule():
    stopServer(server)
    print_module_done(__name__)


class ProtocolBadStartup(unittest.TestCase):

    # setup&teardown client
    def setUp(self):
        global counter
        counter += 1
        print "  running test %d" % counter
        self.server = None
        self.mc = memcache.Client(["%s:%s" % (SERVER, PORT)], debug=0)
        #reset server defaults
        execfile(os.path.expanduser(TESTS_PATH) + '/config/server/default.py')

    def tearDown(self):
        pass # the twemcache instances will kill themselves
    #
    # tests
    #
    def test_badhost(self):
        '''bad host name, -l'''
        args = Args(command='SERVER = "foo"')
        self.server = startServer(args)
        time.sleep(SHUTDOWN_DELAY) # give the process enough time to finish up
        self.server.poll()
        self.assertIsNotNone(self.server.returncode) #termination means error

    def test_badthread(self):
        '''bad thread count, -t'''
        args = Args(command='THREADS = 0')
        self.server = startServer(args)
        time.sleep(SHUTDOWN_DELAY)
        self.server.poll()
        self.assertIsNotNone(self.server.returncode)

    def test_baditemsize(self):
        '''max_item_size too small'''
        args = Args(command = 'SLAB_SIZE = 1000')
        self.server = startServer(args)
        time.sleep(SHUTDOWN_DELAY)
        self.server.poll()
        self.assertIsNotNone(self.server.returncode)

    def test_badaggrintrvl(self):
        '''aggregation interval too larger (> log interval) or negative'''
        args = Args(command='AGGR_INTERVAL = 61000000')
        self.server = startServer(args)
        time.sleep(SHUTDOWN_DELAY)
        self.server.poll()
        self.assertIsNotNone(self.server.returncode)
        args = Args(command='AGGR_INTERVAL = -1')
        self.server = startServer(args)
        time.sleep(SHUTDOWN_DELAY)
        self.server.poll()
        self.assertIsNotNone(self.server.returncode)

    def test_badlog(self):
        '''invalid logging parameters'''
        args = Args(command='LOG_INTERVAL = -1')
        self.server = startServer(args)
        time.sleep(SHUTDOWN_DELAY)
        self.server.poll()
        self.assertIsNotNone(self.server.returncode)
        args = Args(command='LOG_NAME = "%s"' % ('a' * 300))
        self.server = startServer(args)
        time.sleep(SHUTDOWN_DELAY)
        self.server.poll()
        self.assertIsNotNone(self.server.returncode)
        args = Args(command='LOG_NAME = "/////"')
        self.server = startServer(args)
        time.sleep(SHUTDOWN_DELAY)
        self.server.poll()
        self.assertIsNotNone(self.server.returncode)

if __name__ == '__main__':
    protocol_badstartup = unittest.TestLoader().loadTestsFromTestCase(ProtocolBadStartup)
    unittest.TextTestRunner(verbosity=2).run(protocol_badstartup)
