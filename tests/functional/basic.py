__doc__ = '''
Testing the most important commands of twemcache.
Basic functionality test. No expiration, no network latency, no errors.
'''
__author__ = "Yao Yue <yao@twitter.com>"
__version__ = "0.1-1.45"

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
    global server
    server = startServer()

def tearDownModule():
    stopServer(server)
    print_module_done(__name__)


class FunctionalBasic(unittest.TestCase):
    # setup&teardown client
    def setUp(self):
        global counter
        counter += 1
        print "  running test %d" % counter
        self.mc = memcache.Client(
                    ['%s:%s' % (SERVER, PORT)],
                    debug=0)
        self.mc.flush_all()

    def tearDown(self):
        self.mc.flush_all()
        self.mc.disconnect_all()

    #
    # Storage commands
    #

    def test_set(self):
        '''storage: set'''
        self.mc.set("foo", "bar")
        val = self.mc.get("foo")
        self.assertEqual(val, "bar")

    def test_mset(self):
        '''storage: mset'''
        self.mc.set_multi({\
            'megafoo1': 'megabar1',\
            'megafoo2': 'megabar2',\
            'megafoo3': 'megabar3',\
        })
        val = self.mc.get("megafoo1")
        self.assertEqual(val, "megabar1")
        val = self.mc.get("megafoo2")
        self.assertEqual(val, "megabar2")
        val = self.mc.get("megafoo3")
        self.assertEqual(val, "megabar3")

    def test_setflags(self):
        '''storage: set w/ flag'''
        # resort to raw send cmd to get the flags
        server = self.mc.servers[0]
        myflags = [0, 1, 2, 4]
        for myflag in myflags:
            server.send_cmd("set foo %d 0 1\r\n%d" % (myflag, myflag))
            server.readline() # get response of set out of socket buffer
            server.send_cmd("get foo")
            rkey, flags, rlen, = self.mc._expectvalue(server)
            val = server.readline()
            server.readline() # get "END" out of socket buffer
            self.assertEqual(myflag, int(val))
            self.assertEqual(myflag, int(flags))


    def test_add(self):
        '''storage: add'''
        self.mc.set("foo1", "bar1")
        self.mc.add("foo1", "bar2") # should be ignored, as foo1 is already set
        val = self.mc.get("foo1")
        self.assertEqual(val, "bar1")
        self.mc.add("foo2", "bar2")
        val = self.mc.get("foo2")
        self.assertEqual(val, "bar2")

    def test_replace(self):
        '''storage: replace'''
        self.mc.set("foo", "bar")
        self.mc.replace("foo", "rab")
        val = self.mc.get("foo")
        self.assertEqual(val, "rab")
        self.mc.replace("FOO", "BAR") # should be ignored, as foo3 is not set
        val = self.mc.get("FOO")
        self.assertEqual(val, None)

    def test_append(self):
        '''storage: append'''
        self.mc.set("foo", "bar")
        self.mc.append("foo", "BAR")
        val = self.mc.get("foo")
        self.assertEqual(val, "barBAR")
        self.mc.append("foo", "bar")
        val = self.mc.get("foo")
        self.assertEqual(val, "barBARbar")

    def test_prepend(self):
        '''storage: prepend'''
        self.mc.set("foo", "bar")
        self.mc.prepend("foo", "BAR")
        val = self.mc.get("foo")
        self.assertEqual(val, "BARbar")
        self.mc.prepend("foo", "bar")
        val = self.mc.get("foo")
        self.assertEqual(val, "barBARbar")

    def test_mixpend(self):
        '''storage: append mixed with prepend'''
        self.mc.set("foo", "bar")
        self.mc.prepend("foo", "BAR")
        val = self.mc.get("foo")
        self.assertEqual(val, "BARbar")
        self.mc.append("foo", "BAR")
        val = self.mc.get("foo")
        self.assertEqual(val, "BARbarBAR")
        self.mc.prepend("foo", "bar")
        val = self.mc.get("foo")
        self.assertEqual(val, "barBARbarBAR")
        self.mc.append("foo", "bar")
        val = self.mc.get("foo")
        self.assertEqual(val, "barBARbarBARbar")


    def test_cas(self):
        '''storage: cas'''
        self.mc.set("foo", "bar")
        val = self.mc.gets("foo")
        self.assertIsNotNone(self.mc.cas_ids["foo"])
        self.assertEqual(val, "bar")
        self.mc.cas("foo", "barbar")
        val = self.mc.gets("foo")
        self.assertIsNotNone(self.mc.cas_ids["foo"])
        self.assertEqual(val, "barbar")
        anotherMc = memcache.Client(
            ['%s:%s' % (SERVER, PORT)],
            debug=0)
        anotherMc.set("foo", "BAR")
        val = anotherMc.gets("foo")
        self.assertEqual(val, "BAR")
        self.mc.cas("foo", "bar") # should fail and return false
        val = self.mc.gets("foo")
        self.assertIsNotNone(self.mc.cas_ids["foo"])
        self.assertEqual(val, "BAR")
        self.mc.set_multi({"foo": "bar", "FOO": "BAR"})
        self.mc.gets_multi(["foo", "FOO"])

    def test_pipelining(self):
        '''sending multiple commands at once'''
        server = self.mc.servers[0]
        cmds = ["set foo 0 0 3\r\nbar\r\n",
                "set Foo 0 0 3\r\nBar\r\n",
                "set FOo 0 0 3\r\nBAr\r\n",
                "set FOO 0 0 3\r\nBAR\r\n"]
        server.send_cmds(''.join(cmds))
        for cmd in cmds:
            self.assertEqual("STORED", server.readline())
        cmds = ["set foo 0 0 3\r\nbar\r\n",
                "delete foo\r\n",
                "set FOO 0 0 3\r\nBAR\r\n",
                "delete FOO\r\n"]
        server.send_cmds(''.join(cmds))
        self.assertEqual("STORED", server.readline())
        self.assertEqual("DELETED", server.readline())
        self.assertEqual("STORED", server.readline())
        self.assertEqual("DELETED", server.readline())


    #
    # Retrieval
    #

    def test_get(self):
        '''retrieval: get, we call it for other tests, safe to skip here'''
        pass

    def test_mget(self):
        '''retrieval: multi_get'''
        self.mc.set_multi({\
            'megafoo1': 'megabar1',\
            'megafoo2': 'megabar2',\
            'megafoo3': 'megabar3',\
        })
        val = self.mc.get_multi(["megafoo1","megafoo2","megafoo3"])
        self.assertEqual(val, {"megafoo1": "megabar1",\
                               "megafoo2": "megabar2",\
                               "megafoo3": "megabar3"})

    def test_gets(self):
        '''retrieval: gets, we call it for cas tests, safe to skip here'''
        pass

    def test_mget(self):
        '''retrieval: multi_gets'''
        self.mc.set_multi({\
            'megafoo1': 'megabar1',\
            'megafoo2': 'megabar2',\
            'megafoo3': 'megabar3',\
        })
        val = self.mc.gets_multi(["megafoo1","megafoo2","megafoo3"])
        self.assertIsNotNone(self.mc.cas_ids["megafoo1"])
        self.assertIsNotNone(self.mc.cas_ids["megafoo2"])
        self.assertIsNotNone(self.mc.cas_ids["megafoo3"])
        self.assertEqual(val, {"megafoo1": "megabar1",\
                               "megafoo2": "megabar2",\
                               "megafoo3": "megabar3"})

    #
    # Numeric
    #

    def test_incr(self):
        '''numeric: incr'''
        self.mc.set("numkey", "0")
        self.mc.incr("numkey")
        val = self.mc.get("numkey")
        self.assertEqual(val, "1")
        self.mc.incr("numkey", 2)
        val = self.mc.get("numkey")
        self.assertEqual(val, "3")
        self.mc.set("numkey", "9")
        self.mc.incr("numkey")
        val = self.mc.get("numkey")
        self.assertEqual(val, "10")
        self.mc.incr("numkey")
        val = self.mc.get("numkey")
        self.assertEqual(val, "11")

    def test_decr(self):
        '''numeric: decr'''
        self.mc.set("numkey", "3")
        self.mc.decr("numkey")
        val = self.mc.get("numkey")
        self.assertEqual(val, "2")
        self.mc.decr("numkey", 2)
        val = self.mc.get("numkey")
        self.assertEqual(val, "0")

    #
    # Removal
    #

    def test_delete(self):
        '''removal: delete'''
        self.mc.set("foo", "bar")
        val = self.mc.get("foo")
        self.assertEqual(val, "bar")
        self.mc.delete("foo")
        val = self.mc.get("foo")
        self.assertEqual(val, None)

    def test_mdelete(self):
        '''removal: mdelete'''
        self.mc.set_multi({\
            'megafoo1': 'megabar1',\
            'megafoo2': 'megabar2',\
            'megafoo3': 'megabar3',\
        })
        self.mc.delete_multi(["megafoo1", "megafoo2", "megafoo3"])
        val = self.mc.get_multi(["megafoo1", "megafoo2", "megafoo3"])
        self.assertEqual(val, {})

    def test_flush_all(self):
        '''removal: flush_all'''
        self.mc.set("foo", "bar")
        self.mc.set("FOO", "BAR")
        self.mc.flush_all()
        val = self.mc.get_multi(["foo", "FOO"]) # should have expired
        self.assertEqual(val, {})

    #
    # Misc: noreply, etc
    #

    def test_noreply(self):
        ''' noreply mode'''
        server = self.mc.servers[0]
        server.connect()
        server.send_cmd("add foo 0 0 1 noreply\r\n1")
        self.assertEqual("1", self.mc.get("foo"))
        server.send_cmd("set foo 0 0 1 noreply\r\n2")
        self.assertEqual("2", self.mc.get("foo"))
        server.send_cmd("replace foo 0 0 1 noreply\r\n3")
        self.assertEqual("3", self.mc.get("foo"))
        server.send_cmd("append foo 0 0 1 noreply\r\n4")
        self.assertEqual("34", self.mc.get("foo"))
        server.send_cmd("prepend foo 0 0 1 noreply\r\n5")
        self.assertEqual("534", self.mc.gets("foo"))
        server.send_cmd("cas foo 0 0 1 %d noreply\r\n6" % self.mc.cas_ids['foo'])
        self.assertEqual("6", self.mc.get("foo"))
        server.send_cmd("incr foo 3 noreply")
        self.assertEqual("9", self.mc.get("foo"))
        server.send_cmd("decr foo 2 noreply")
        self.assertEqual("7", self.mc.get("foo"))
        server.send_cmd("delete foo noreply")
        self.assertEqual(None, self.mc.get("foo"))


if __name__ == '__main__':
    functional_basic = unittest.TestLoader().loadTestsFromTestCase(FunctionalBasic)
    unittest.TextTestRunner(verbosity=2).run(functional_basic)
