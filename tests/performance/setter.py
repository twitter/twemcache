__doc__='''
Launch a group of identical setters
'''

import sys
import random
import os
import signal
import subprocess
from multiprocessing import Process

from lib.utilities import *
from lib.common import Client, Data

DURATION = 20
FREQ = 1000
WORKERS = int(sys.argv[-1])

class args:
    config = os.path.expanduser(TESTS_PATH)+'/config/server/mem4mb.py'
#server = subprocess.Popen(['strace', '-c', 'twemcache'])
#server = subprocess.Popen(['ltrace', '-c', 'twemcache'])
#server = subprocess.Popen(['mutrace', '-d', os.path.expanduser(TESTS_PATH)+'../twemcache', '-t 64'])
server = startServer(args)
data = Data(os.path.expanduser(TESTS_PATH)+'/data/data.config')

def worker(data):
    mc = Client(data, freq=FREQ)
    mc.setter_random_random(DURATION)

workers = {}
for i in range(0, WORKERS):
    workers[i] = Process(target=worker, args=[data,])

print "Starting workers."
for i in range(0, WORKERS):
    workers[i].start()

print "Waiting for workers."
for i in range(0, WORKERS):
    workers[i].join()

#os.kill(server.pid, signal.SIGINT) #strace
#subprocess.Popen(['pkill', '-SIGINT', 'twemcache']) #ltrace
#os.kill(server.pid, signal.SIGINT) #mutrace
stopServer(server)
