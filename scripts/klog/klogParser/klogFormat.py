__doc__ =  '''
Defines valid log format / pattern.
  cmd_pattern: normal command log entry format
  lgd_pattern: log discarding entry format

  hdr_patterns: a dictionary of patterns covering header formats of all the commands

'''

import re

_cmd_parts = [
  r'(?P<host>\S+)',                   # host %h
  r'\S+',                             # indent %l (unused)
  r'\[(?P<time>.+)\]',                # time %t
  r'"(?P<header>.+)"',               # request "%r"
  r'(?P<status>[0-9]+)',              # status %>s
  r'(?P<size>\d+)'                    # size %b
]
cmd_pattern = re.compile(r' '.join(_cmd_parts)+r'\s*\Z')

lgd_pattern = re.compile(r'(?P<num>\d+) logs discarded\s*\Z') # logs discarded

_req_len7 = ['cas']
_req_len6 = ['add', 'set', 'replace', 'append', 'prepend']
_req_len4 = ['incr', 'decr']
_req_len3 = ['delete']
_req_len2 = ['get', 'gets']

requests = _req_len7 + _req_len6 + _req_len4 + _req_len3 + _req_len2

hdr_patterns = {}

for req in _req_len7:
  hdr_patterns[req] = re.compile(' '.join([req, r'(?P<key>\S+) (?P<exptime>\d+) (?P<flag>\d+) (?P<bytes>\d+) (?P<cas>\d+)(?P<noreply> noreply)?\s*\Z']))
for req in _req_len6:
  hdr_patterns[req] = re.compile(' '.join([req, r'(?P<key>\S+) (?P<exptime>\d+) (?P<flag>\d+) (?P<bytes>\d+)(?P<noreply> noreply)?\s*\Z']))
for req in _req_len4:
  hdr_patterns[req] = re.compile(' '.join([req, r'(?P<key>\S+) (?P<value>\d+)(?P<noreply> noreply)?\s*\Z']))
for req in _req_len3:
  hdr_patterns[req] = re.compile(' '.join([req, r'(?P<key>\S+)(?P<noreply> noreply)?\s*\Z']))
for req in _req_len2:
  hdr_patterns[req] = re.compile(' '.join([req, r'(?P<key>\S+)\s*\Z']))
