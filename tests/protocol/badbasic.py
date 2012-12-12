__doc__ = '''
Testing when various basic commands meet bad parameters.
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

def setUpModule():
    print_module_title(__name__)
    global counter
    counter = 0

def tearDownModule():
    print_module_done(__name__)


class ProtocolBadBasic(unittest.TestCase):

    # setup&teardown client/server
    def setUp(self):
        global counter
        counter += 1
        print "  running test %d" % counter
        global server
        server = startServer()
        self.server = None
        self.mc = memcache.Client(["%s:%s" % (SERVER, PORT)], debug=0)

    def tearDown(self):
        stopServer(server)

    #
    # tests
    #
    def test_badcas1(self):
        '''cas goes wild: cas id missing.'''
        # coz memcache.py is trying to be smart with cas, we have to send it raw
        server = self.mc.servers[0]
        server.connect()
        server.send_cmd("cas foo 0 0 3\r\nbar") # missing cas id
        self.assertEqual("CLIENT_ERROR", server.expect("CLIENT_ERROR"))
        val = self.mc.get("foo")
        self.assertIsNone(val)

    def test_badcas2(self):
        '''cas goes wild: arbitrary cas id.'''
        server = self.mc.servers[0]
        server.connect()
        server.send_cmd("cas foo 0 0 3 123\r\nbar") # arbitrary cas id
        self.assertEqual("NOT_FOUND", server.expect("NOT_FOUND"))
        val = self.mc.get("foo")
        self.assertIsNone(val)

    def test_badcas3(self):
        '''cas goes wild: reusing cas id.'''
        server = self.mc.servers[0]
        server.connect()
        self.mc.set("foo", "bar")
        val = self.mc.gets("foo")
        casid = self.mc.cas_ids["foo"]
        server.send_cmd("cas foo 0 0 3 %d\r\nBar" % casid) # should succeed
        self.assertEqual("STORED", server.expect("STORED"))
        time.sleep(0.5) # to avoid racing
        server.send_cmd("cas foo 0 0 3 %d\r\nBAr" % casid) # reuse cas id
        self.assertEqual("EXISTS", server.expect("EXISTS"))
        val = self.mc.gets("foo")
        self.assertEqual("Bar", val)

    def test_longkey(self):
        '''key is too long.'''
        server = self.mc.servers[0]
        server.connect()
        key_len = 500
        key = 'a' * key_len
        server.send_cmd("get {0}\r\n".format(key)) # looooooong key
        self.assertEqual("CLIENT_ERROR", server.expect("CLIENT_ERROR"))

    def test_largevalue(self):
        '''Append/prepend grows item out of size range.'''
        key = 'appendto'
        val = '0' * (SLAB_SIZE - SLAB_OVERHEAD - ITEM_OVERHEAD - 100)
        self.mc.set(key, val)
        self.assertEqual(val, self.mc.get(key))
        delta = '0' * 100
        self.mc.append(key, delta) # delta makes the value out of range, no-op expected
        self.assertEqual(val, self.mc.get(key))
        self.mc.prepend(key, delta) # delta makes the value out of range, no-op expected
        self.assertEqual(val, self.mc.get(key))

if __name__ == '__main__':
    server = startServer()
    protocol_basic = unittest.TestLoader().loadTestsFromTestCase(ProtocolBadBasic)
    unittest.TextTestRunner(verbosity=2).run(protocol_basic)
    stopServer(server)
