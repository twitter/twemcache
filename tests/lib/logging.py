__doc__ = '''
Logger for testing.
'''
__author__ = "Yao Yue <yao@twitter.com>"
__version__ = "0.1-1.45"

import sys


def print_newline():
    print("")

def print_title(s):
    fmt = "=========={0:^30}=========="
    print(fmt.format(s))

def print_section(s):
    fmt = "----------{0:^30}----------"
    print(fmt.format(s))

def print_module_title(s):
    print_section("Module: " + s)

def print_module_done(s):
    print_section("Done: " + s)

def print_result(result):
    print('Tests run: {0:2d}'.format(result.testsRun))
    print('Skipped:   {0:2d}'.format(len(result.skipped)))
    print('Failures:  {0:2d}'.format(len(result.failures)))
    print('Errors:    {0:2d}'.format(len(result.errors)))

def print_verbose(s,v):
    if v:
        print(s)

