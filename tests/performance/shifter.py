__doc__='''
Forking off two test runners.
Both runners execute the 'set' command.
One shifts from the smallest bucket to the largest.
The other sleeps for a while and does the shifting in reverse order.
'''

import sys
import os
import random
import time
import subprocess
from multiprocessing import Process

from config import defaults
from lib.utilities import *
from lib.common import Client
from lib.common import Data

LOGFILE = '/shifter.log'
SERVER_CONFIG = '/config/server/mem4mb.py'
DATAFILE1 = '/data/data.low'
DATAFILE2 = '/data/data.high'
PREFIX = os.path.expanduser(TESTS_PATH)
CMD_FREQ = 200
DURATION = 5

def runner_a():
    data = Data(PREFIX+DATAFILE1)
    mc = Client(data, freq=CMD_FREQ)
    for bucket in sorted(data.buckets):
        mc.setter_fixed_random(bucket, DURATION)

def runner_b():
    data = Data(PREFIX+DATAFILE2)
    mc = Client(data, freq=CMD_FREQ)
    time.sleep(DURATION * len(data.buckets) * 2)
    for bucket in sorted(data.buckets):
        mc.setter_fixed_random(bucket, DURATION)

class args:
    config = PREFIX + SERVER_CONFIG
server = startServer(args)

p_runnera = Process(target=runner_a)
p_runnerb = Process(target=runner_b)

time.sleep(5)

print "Starting runners, type a.\n"
p_runnera.start()
print "Starting runners, type b.\n"
p_runnerb.start()

print "Waiting for runners, type a.\n"
p_runnera.join()
print "Waiting for runners, type b.\n"
p_runnerb.join()

time.sleep(20)
stopServer(server)
