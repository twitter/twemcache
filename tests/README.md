## Environment

Tests are mostly written in Python, and runs on top of python-memcache (latest version is 1.45). Set PYTHONPATH to include the path of 'tests' directory in your shell script.

## Testing Methodology

Tests are divided into three catogeries:
* Functional Tests (Cover the Basics, part of unittest & 'make test')
* Protocol/Corner Cases (Errors and Anomalies, part of unittest & 'make test')
* Performance & Customized Tests

## Test Atomicity

Each test module launches its twemcache server instance, and kills it afterwards. There is no dependency between test modules, so they can run in any order/combination.

Customized tests can build on common client-side classes which provide basic client behavior (in common.py).

## Configuration

The test environment, such as server settings, is read from a Python module in the config sub-directory. For unittests, using the default configurations should suffice. However, the separation makes it easy to tweak the setting if one wants to, or create a new config for a customized test.

'make test' will automatically create config files from templates and use them in unittesting. The configuration templates include:
* config/defaults-template.py: the default value to hightest level arguments
* config/server/default-template.py: default values used to set up and test a server
* config/data/default-template.py: default values for tests that needs to generate a large number of (key,value) pairs.

## Notes

Tests for expiry items run rather slowly compared to the others. This is because expiration granularity in Twemcache (and Memcached as well for that matter) is one second. So the tests should allow that much time before verifying the results on expiry items.
