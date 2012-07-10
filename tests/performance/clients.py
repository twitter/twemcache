__doc__='''
Test out all Client methods.
'''

import sys
import os.path
import random

from lib.utilities import *
from lib.common import Client, Data

DURATION = 5
FREQ = 100

server = startServer()
data = Data(os.path.expanduser(TESTS_PATH)+'/data/data.config')

mc = Client(data, freq=FREQ)

bucket = data.buckets[random.randrange(len(data.buckets))]
print"Testing setter_fixed_single"
mc.setter_fixed_single(bucket, 1, DURATION)
print"Testing getter_fixed_single"
mc.getter_fixed_single(bucket, 1, DURATION)
bucket = data.buckets[random.randrange(len(data.buckets))]
print"Testing setter_fixed_random"
mc.setter_fixed_random(bucket, DURATION)
print"Testing getter_fixed_random"
mc.getter_fixed_random(bucket, DURATION)
bucket = data.buckets[random.randrange(len(data.buckets))]
print"Testing setter_fixed_unbounded"
mc.setter_fixed_unbounded(bucket, DURATION)

print"Testing setter_random_random"
mc.setter_random_random(DURATION)
print"Testing setter_random_unbounded"
mc.setter_random_unbounded(DURATION)

mc.setter_filler()

bucket = data.buckets[random.randrange(len(data.buckets))]
print"Testing appender_fixed_random"
mc.appender_fixed_random(bucket, 32, DURATION)
print"Testing appender_random_random"
mc.appender_random_random(32, DURATION)
bucket = data.buckets[random.randrange(len(data.buckets))]
print"Testing prepender_fixed_random"
mc.prepender_fixed_random(bucket, 32, DURATION)
print"Testing prepender_random_random"
mc.prepender_random_random(32, DURATION)

print"Testing getter_random_random"
mc.getter_random_random(DURATION)
print"Testing getter_unbounded"
mc.getter_unbounded(DURATION)

stopServer(server)
