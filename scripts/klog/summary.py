from __future__ import print_function

from klogParser import klogFormat
import argparse

def match_request(header):
  for req in klogFormat.hdr_patterns:
    if klogFormat.hdr_patterns[req].match(header):
      req_summary[req] += 1
      return
  req_summary['others'] += 1


parser = argparse.ArgumentParser(description='Take a klog file and generate a summary report.')
parser.add_argument('-f', '--logname', dest='logname', metavar='LOG FILE', default='',
                    nargs='?', help='log file name, e.g. key.log', required=True)
args = parser.parse_args()

log_file = open(args.logname, 'r')

summary = {'cmd':0, 'err':0}
summary_formats = {
  'cmd':'{entries:>10d} command logged',
  'err':'{entries:>10d} lines cannot be recognized'
  }
req_summary = dict([(req, 0) for req in klogFormat.requests + ['others'] ])
req_summary_format = 'Number of command {req:>9s}: {entries:>10d}'

for line in log_file:
  cmd_match = klogFormat.cmd_pattern.match(line)
  if cmd_match:
    summary['cmd'] += 1
    match_request(cmd_match.groupdict()['header'])
  else:
    summary['err'] += 1

title_format = '\n==={title:^s}===\n'
print(title_format.format(title='Log Summary'))
for entry_type in summary:
  print(summary_formats[entry_type].format(entries=summary[entry_type]))

print(title_format.format(title='Command Summary'))
for req_type in req_summary:
  print(req_summary_format.format(req=req_type, entries=req_summary[req_type]))
