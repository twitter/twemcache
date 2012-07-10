__doc__ = '''
Testing the most important commands of twemcache.
Functionality test with expiration timestamp.
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
TIMER_LONG = 1
TIMER_LLONG = TIMER_LONG + 1

def setUpModule():
    print_module_title(__name__)
    global counter
    counter = 0

def tearDownModule():
    print_module_done(__name__)


class FunctionalExpiry(unittest.TestCase):

    # setup&teardown client
    def setUp(self):
        global counter
        counter += 1
        global server
        server = startServer()
        print "  running test %d" % counter
        self.mc = memcache.Client(['%s:%s' % (SERVER, PORT)], debug=0)

    def tearDown(self):
        self.mc.disconnect_all()
        stopServer(server)

    #
    # Storage commands
    #

    def test_set(self):
        '''storage: set'''
        self.mc.set("foo", "bar", TIMER_LONG)
        time.sleep(TIMER_SHORT)
        val = self.mc.get("foo")
        self.assertEqual(val, "bar")
        time.sleep(TIMER_LLONG)
        val = self.mc.get("foo")
        self.assertEqual(val, None)

    def test_mset(self):
        '''storage: mset'''
        self.mc.set_multi({\
            'megafoo1': 'megabar1',\
            'megafoo2': 'megabar2',\
            'megafoo3': 'megabar3',\
        }, TIMER_LONG)
        time.sleep(TIMER_SHORT)
        val = self.mc.get("megafoo1")
        self.assertEqual(val, "megabar1")
        val = self.mc.get("megafoo2")
        self.assertEqual(val, "megabar2")
        val = self.mc.get("megafoo3")
        self.assertEqual(val, "megabar3")
        time.sleep(TIMER_LLONG)
        val = self.mc.get("megafoo1")
        val = val or self.mc.get("megafoo2")
        val = val or self.mc.get("megafoo3")
        self.assertEqual(val, None)


    def test_add(self):
        '''storage: add'''
        self.mc.set("foo", "bar", TIMER_LONG)
        self.mc.add("foo", "BAR") # should be ignored, as foo1 is already set
        time.sleep(TIMER_SHORT)
        val = self.mc.get("foo")
        self.assertEqual(val, "bar")
        time.sleep(TIMER_LLONG)
        self.mc.add("foo", "BAR", TIMER_LONG) # previous entry timed out
        val = self.mc.get("foo")
        self.assertEqual(val, "BAR")

    def test_replace(self):
        '''storage: replace'''
        self.mc.set("foo", "bar", TIMER_LONG)
        time.sleep(TIMER_SHORT)
        self.mc.replace("foo", "rab", TIMER_LONG)
        val = self.mc.get("foo")
        self.assertEqual(val, "rab")
        time.sleep(TIMER_LLONG)
        self.mc.replace("foo", "BAR", TIMER_LONG) # should be ignored, foo has timed out
        val = self.mc.get("foo")
        self.assertEqual(val, None)

    def test_append(self):
        '''storage: append'''
        self.mc.set("foo", "bar", TIMER_LONG)
        time.sleep(TIMER_SHORT)
        self.mc.append("foo", "BAR", TIMER_LONG)
        val = self.mc.get("foo")
        self.assertEqual(val, "barBAR")
        time.sleep(TIMER_LLONG)
        self.mc.append("foo", "BAR") # should be ignored, foo has timed out
        val = self.mc.get("foo")
        self.assertEqual(val, None)

    def test_prepend(self):
        '''storage: prepend'''
        self.mc.set("foo", "bar", TIMER_LONG)
        time.sleep(TIMER_SHORT)
        self.mc.prepend("foo", "BAR", TIMER_LONG)
        val = self.mc.get("foo")
        self.assertEqual(val, "BARbar")
        time.sleep(TIMER_LLONG)
        self.mc.prepend("foo", "BAR") # should be ignored, foo has timed out
        val = self.mc.get("foo")
        self.assertEqual(val, None)


    def test_cas(self):
        '''storage: cas'''
        self.mc.cas("foo", "bar", TIMER_LONG)
        val = self.mc.gets("foo")
        self.assertEqual(val, "bar")
        time.sleep(TIMER_SHORT)
        self.mc.cas("foo", "barbar", TIMER_LONG)
        val = self.mc.gets("foo")
        self.assertEqual(val, "barbar")
        anotherMc = memcache.Client(
            ['%s:%s' % (SERVER, PORT)],
            debug=0)
        anotherMc.cas("foo", "BAR", TIMER_LONG)
        val = anotherMc.gets("foo")
        self.assertEqual(val, "BAR")
        time.sleep(TIMER_SHORT)
        self.mc.cas("foo", "bar", TIMER_LONG) # should fail and return false
        val = self.mc.get("foo")
        self.assertEqual(val, "BAR")
        time.sleep(TIMER_LLONG)
        self.mc.cas("foo", "bar", TIMER_LONG) # should fail as entry expired
        val = self.mc.get("foo")
        self.assertEqual(val, None)


    #
    # Retrieval
    #

    def test_get(self):
        '''retrieval: get, we call it for other tests, safe to skip here'''
        pass

    def test_gets(self):
        '''retrieval: gets'''
        self.mc.set_multi({\
            'megafoo1': 'megabar1',\
            'megafoo2': 'megabar2',\
            'megafoo3': 'megabar3',\
        }, TIMER_LONG)
        time.sleep(TIMER_SHORT)
        val = self.mc.get_multi(["megafoo1","megafoo2","megafoo3"])
        self.assertEqual(val, {"megafoo1": "megabar1",\
                               "megafoo2": "megabar2",\
                               "megafoo3": "megabar3"})
        time.sleep(TIMER_LLONG)
        val = self.mc.get_multi(["megafoo1","megafoo2","megafoo3"])
        self.assertEqual(val, {})

    #
    # Numeric
    #

    def test_incr(self):
        '''numeric: incr'''
        self.mc.set("numkey", "0", TIMER_LONG)
        time.sleep(TIMER_SHORT)
        self.mc.incr("numkey")
        val = self.mc.get("numkey")
        self.assertEqual(val, "1")
        time.sleep(TIMER_LLONG)
        self.mc.incr("numkey", 2) # should be ignored, numkey has timed out
        val = self.mc.get("numkey")
        self.assertEqual(val, None)

    def test_decr(self):
        '''numeric: decr'''
        self.mc.set("numkey", "3", TIMER_LONG)
        time.sleep(TIMER_SHORT)
        self.mc.decr("numkey")
        val = self.mc.get("numkey")
        self.assertEqual(val, "2")
        time.sleep(TIMER_LLONG)
        self.mc.decr("numkey", 2) # should be ignored, numkey has timed out
        val = self.mc.get("numkey")
        self.assertEqual(val, None)

    #
    # Removal
    #

    def test_delete(self):
        '''removal: delete'''
        pass # nothing new to test

#    def test_mdelete(self):
#        '''removal: mdelete'''
#        self.mc.set_multi({\
#            'megafoo1': 'megabar1',\
#            'megafoo2': 'megabar2',\
#            'megafoo3': 'megabar3',\
#        })
#        self.mc.delete_multi(["megafoo1", "megafoo2", "megafoo3"])
#        val = self.mc.get_multi(["megafoo1", "megafoo2", "megafoo3"])
#        self.assertEqual(val, {})

    def test_flush_all(self):
        '''removal: flush_all'''
        self.mc.set("foo", "bar")
        self.mc.set("FOO", "BAR")
        self.mc.flush_all(TIMER_LLONG)
        time.sleep(TIMER_SHORT)
        val = self.mc.get_multi(["foo", "FOO"]) # still around
        self.assertEqual(val, {"foo":"bar", "FOO":"BAR"})
        time.sleep(TIMER_LLONG)
        val = self.mc.get_multi(["foo", "FOO"]) # should have expired
        self.assertEqual(val, {})


if __name__ == '__main__':
    functional_expiry = unittest.TestLoader().loadTestsFromTestCase(FunctionalExpiry)
    unittest.TextTestRunner(verbosity=2).run(functional_expiry)
