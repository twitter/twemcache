__doc__ = '''
Testing stats related commands.
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

# handling server and data configurations
from config.defaults import *
from lib.utilities import *
from lib.logging import print_module_title, print_module_done

TIMER_SHORT = 0.1

def setUpModule():
    print_module_title(__name__)
    global counter
    counter = 0

def tearDownModule():
    print_module_done(__name__)


class FunctionalStats(unittest.TestCase):

    # setup&teardown client
    def setUp(self):
        global counter
        counter += 1
        global server
        server = startServer()
        print "  running test %d" % counter
        self.mc = memcache.Client(
                    ['%s:%s' % (SERVER, PORT)],
                    debug=0)

    def tearDown(self):
        self.mc.disconnect_all()
        stopServer(server)

    #
    # Storage commands
    #

    def test_00init(self):
        '''inital stats, prepended with 00 to make sure it runs first'''
        stats = self.mc.get_stats()
        self.assertEqual(1, len(stats))
        stats = stats[0][1]
        self.assertEqual(len(STATS_KEYS), len(stats))

    def test_setget(self):
        ''' set and get related stats'''
        self.mc.set("foo", "bar")
        stats = self.mc.get_stats()[0][1]
        self.assertEqual("1", stats['set'])
        self.assertEqual("1", stats['set_success'])
        self.mc.get("foo")
        stats = self.mc.get_stats()[0][1]
        self.assertEqual("1", stats['get'])
        self.assertEqual("1", stats['get_hit'])
        self.mc.get("NOFOO") # get miss, make sure NOFOO isn't set elsewhere
        stats = self.mc.get_stats()[0][1]
        self.assertEqual("2", stats['get'])
        self.assertEqual("1", stats['get_hit'])
        self.assertEqual("1", stats['get_miss'])

    def test_delete(self):
        '''delete'''
        self.mc.set("Foo", "Bar")
        self.mc.set("FOo", "BAr")
        self.mc.delete("Foo")
        stats = self.mc.get_stats()[0][1]
        self.assertEqual("1", stats['delete'])
        self.assertEqual("1", stats['delete_hit'])
        self.assertEqual("0", stats['delete_miss'])
        self.mc.delete("NOFOO")
        stats = self.mc.get_stats()[0][1]
        self.assertEqual("2", stats['delete'])
        self.assertEqual("1", stats['delete_hit'])
        self.assertEqual("1", stats['delete_miss'])

    def test_incrdecr(self):
        ''' incr and decr related stats'''
        # misses
        stats = self.mc.get_stats()[0][1]
        self.assertEqual("0", stats['incr_miss'])
        self.mc.incr("n", 1)
        stats = self.mc.get_stats()[0][1]
        self.assertEqual("1", stats['incr'])
        self.assertEqual("1", stats['incr_miss'])

        self.assertEqual("0", stats['decr_miss'])
        self.mc.decr("n", 1)
        stats = self.mc.get_stats()[0][1]
        self.assertEqual("1", stats['decr'])
        self.assertEqual("1", stats['decr_miss'])
        # hits
        self.mc.set("u", 1)
        stats = self.mc.get_stats()[0][1]
        self.assertEqual("0", stats['incr_hit'])
        self.mc.incr("u", 1)
        stats = self.mc.get_stats()[0][1]
        self.assertEqual("2", stats['incr'])
        self.assertEqual("1", stats['incr_hit'])
        self.assertEqual("1", stats['incr_miss'])
        self.assertEqual("1", stats['incr_success'])
        stats = self.mc.get_stats()[0][1]
        self.assertEqual("0", stats['decr_hit'])
        self.mc.decr("u", 1)
        stats = self.mc.get_stats()[0][1]
        self.assertEqual("2", stats['decr'])
        self.assertEqual("1", stats['decr_hit'])
        self.assertEqual("1", stats['decr_miss'])
        self.assertEqual("1", stats['decr_success'])

    def test_casgets(self):
        ''' cas stats '''
        self.mc.set("foo", "bar")
        # gets
        self.mc.gets("FOO")
        stats = self.mc.get_stats()[0][1]
        self.assertEqual("1", stats['gets'])
        self.assertEqual("1", stats['gets_miss'])
        self.mc.gets("foo")
        # what's werid here is gets create a new item internally,
        # leading to a higher total_items count
        stats = self.mc.get_stats()[0][1]
        self.assertEqual("2", stats['gets'])
        self.assertEqual("1", stats['gets_hit'])
        self.assertEqual("1", stats['gets_miss'])
        # cas
        casid = self.mc.cas_ids["foo"]
        self.mc.cas("foo", "barbar")
        stats = self.mc.get_stats()[0][1]
        self.assertEqual("1", stats['cas'])
        self.assertEqual("1", stats['cas_hit'])
        self.assertEqual("1", stats['cas_success'])
        server = self.mc.servers[0]
        server.send_cmd("cas FOO 0 0 6 12345\r\nbarbar")
        server.expect("NOT_FOUND")
        stats = self.mc.get_stats()[0][1]
        self.assertEqual("2", stats['cas'])
        self.assertEqual("1", stats['cas_miss'])
        server.send_cmd("cas foo 0 0 3 %d\r\nbar" % casid)
        server.expect("EXISTS")
        stats = self.mc.get_stats()[0][1]
        self.assertEqual("3", stats['cas'])
        self.assertEqual("1", stats['cas_hit'])
        self.assertEqual("1", stats['cas_miss'])
        self.assertEqual("1", stats['cas_badval'])
        self.assertEqual("1", stats['cas_success'])

    def test_replace(self):
        '''replace'''
        self.mc.replace("FOO", "BAR")
        stats = self.mc.get_stats()[0][1]
        self.assertEqual("1", stats['replace'])
        self.assertEqual("1", stats['replace_miss'])
        self.mc.set("foo", "bar")
        self.mc.replace("foo", "BAR")
        stats = self.mc.get_stats()[0][1]
        self.assertEqual("2", stats['replace'])
        self.assertEqual("1", stats['replace_hit'])
        self.assertEqual("1", stats['replace_miss'])
        self.assertEqual("1", stats['replace_success'])

    def test_add(self):
        '''add'''
        self.mc.add("foo", "bar")
        stats = self.mc.get_stats()[0][1]
        self.assertEqual("1", stats['add'])
        self.assertEqual("1", stats['add_success'])
        self.mc.add("foo", "BAR")
        stats = self.mc.get_stats()[0][1]
        self.assertEqual("2", stats['add'])
        self.assertEqual("1", stats['add_exist'])
        self.assertEqual("1", stats['add_success'])

    def test_appendprepend(self):
        '''append/prepend'''
        self.mc.append("foo", "bar")
        stats = self.mc.get_stats()[0][1]
        self.assertEqual("1", stats['append'])
        self.assertEqual("1", stats['append_miss'])
        self.mc.set("foo", "bar")
        self.mc.append("foo", "BAR")
        stats = self.mc.get_stats()[0][1]
        self.assertEqual("2", stats['append'])
        self.assertEqual("1", stats['append_hit'])
        self.assertEqual("1", stats['append_miss'])
        self.assertEqual("1", stats['append_success'])
        self.mc.prepend("FOO", "bar")
        stats = self.mc.get_stats()[0][1]
        self.assertEqual("1", stats['prepend'])
        self.assertEqual("1", stats['prepend_miss'])
        self.mc.prepend("foo", "BAR")
        stats = self.mc.get_stats()[0][1]
        self.assertEqual("2", stats['prepend'])
        self.assertEqual("1", stats['prepend_hit'])
        self.assertEqual("1", stats['prepend_miss'])
        self.assertEqual("1", stats['prepend_success'])


if __name__ == '__main__':
    functional_stats = unittest.TestLoader().loadTestsFromTestCase(FunctionalStats)
    unittest.TextTestRunner(verbosity=2).run(functional_stats)
