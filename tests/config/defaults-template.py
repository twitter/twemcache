__doc__ = '''
Constants and Environment settings.
'''
__author__ = "Yao Yue <yao@twitter.com>"
__version__ = "0.1-1.45"

import os

TESTS_PATH = os.getcwd() + '/tests'

VERBOSE = False

# default server configuration
from server.default import *

# data configuration
from data.default import *

# client configuration
FREQUENCY = 20 # operative requests per second
STATS_FREQ = 0.5 # stats request per second
STATS_LOG = "%s-%s.log" % (SERVER, PORT)
