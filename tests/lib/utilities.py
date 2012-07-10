__doc__ = '''
Utilities for trivial tasks such as flush twemcache server status, etc.
'''
__author__ = "Yao Yue <yao@twitter.com>"
__version__ = "0.1-1.45"

import time
import sys
import subprocess
try:
    from lib import memcache
except ImportError:
    print "Place memcache module (version 1.45) in lib."
    sys.exit()
try:
    import argparse
except ImportError:
    print "Please download argparse first. It is native in Python 2.7 & 3.x."
    sys.exit()
from lib.logging import print_verbose
from config.defaults import *

class Args(object):
    def __init__(self, config=None, command=None):
        self.config = config
        self.command = command

def startServer(args=None):
    '''launch twemcache with a config file'''
    if args:
        if hasattr(args, 'config') and args.config:
            execfile(args.config)
        elif hasattr(args, 'command') and args.command:
            exec(args.command)
    options = [EXEC]
    for key in ARGS_UNARY.keys():
        if eval(key):
            options.append(ARGS_UNARY[key])
    for key in ARGS_BINARY.keys():
        if eval(key) != None:
            options.append(ARGS_BINARY[key])
            options.append(str(eval(key)))
    server = subprocess.Popen(options)
    time.sleep(INIT_DELAY)
    print_verbose("parameters: " + ' '.join(options), VERBOSE)
    print_verbose("Started twemcache instance, pid %d\n" % server.pid, VERBOSE)
    return server

def stopServer(server=None):
    '''shutdown twemcache'''
    if server != None:
        server.kill()
        time.sleep(SHUTDOWN_DELAY)
        print_verbose("Stopped twemcache instance, pid %d\n" % server.pid, VERBOSE)


def cleanup(args):
    '''flush a server.'''
    mc = memcache.Client(['%s:%s' % (args.server, args.port)], debug=0)
    mc.flush_all()
    mc.disconnect_all()

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='Choose a command')
    subparsers = parser.add_subparsers()
    parser_cleanup = subparsers.add_parser('cleanup')
    parser_cleanup.set_defaults(func=cleanup)
    parser_cleanup.add_argument('-s', '--server',
                                dest='server',
                                default=SERVER,
                                nargs='?')
    parser_cleanup.add_argument('-p', '--port',
                                dest='port',
                                default=PORT,
                                nargs='?')

    parser_start = subparsers.add_parser('start')
    parser_start.set_defaults(func=startServer)
    parser_start.add_argument('-c', '--config',
                                dest='config',
                                default=None,
                                nargs='?')

    args = parser.parse_args()
    args.func(args)
