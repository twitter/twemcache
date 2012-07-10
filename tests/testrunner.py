#!/usr/bin/python

from __future__ import print_function
import sys
try:
    import unittest2 as unittest
except ImportError:
    import unittest
from lib.logging import *

if '-q' in sys.argv:
    sys.stdout = open("/dev/null", "a") # supress customized output within the test

# functional tests
import functional
functional_loader = unittest.TestLoader()
functional_result = unittest.TestResult()
functional_suite = unittest.TestSuite()
for test in functional.__all__:
    functional_suite.addTests(
        functional_loader.loadTestsFromName('.'.join(['functional', test])))
functional_suite.run(functional_result)

# protocol tests
import protocol
protocol_loader = unittest.TestLoader()
protocol_result = unittest.TestResult()
protocol_suite = unittest.TestSuite()
for test in protocol.__all__:
    protocol_suite.addTests(
        protocol_loader.loadTestsFromName('.'.join(['protocol', test])))
protocol_suite.run(protocol_result)

sys.stdout = sys.__stdout__ # restore default stdout

# Summerize results

print_newline()
print_title("Functional Tests")
print_result(functional_result)
if functional_result.wasSuccessful():
    print_title("Functional: PASS")
else:
    print_title("Functional: FAIL")
    print_section('Errors')
    for error in functional_result.errors:
        print(error[0])
        print(error[1])
        print_newline()
    print_section('Failures')
    for failure in functional_result.failures:
        print(failure[0])
        print(failure[1])
        print_newline()

print_newline()
print_title("Protocol Tests")
print_result(protocol_result)
if protocol_result.wasSuccessful():
    print_title("Protocol: PASS")
else:
    print_title("Protocol: FAIL")
    print_section('Errors')
    for error in protocol_result.errors:
        print(error[0])
        print(error[1])
        print_newline()
    print_section('Failures')
    for failure in protocol_result.failures:
        print(failure[0])
        print(failure[1])
        print_newline()

print_newline()
print_title("Summary")
if (functional_result.wasSuccessful() and protocol_result.wasSuccessful()):
    print_title("PASS")
else:
    print_title("FAIL")
