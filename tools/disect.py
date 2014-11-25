#!/usr/bin/env python2

import os, sys, time, re, getopt, subprocess, env_setup
sys.path.extend([ env_setup.benchmarks_root(), os.path.join(env_setup.benchmarks_root(), 'tools', 'scheduler') ])
import iqclient, iqlib, intelqueue

ic = iqclient.IntelClient()


def ex_ret(cmd):
  return subprocess.Popen([ 'bash', '-c', cmd ], stdout = subprocess.PIPE).communicate()[0]
def git(args):
  return ex_ret('git --git-dir="%s/.git" %s' % (env_setup.sim_root(),args)).strip()


if len(sys.argv) < 3:
  prefix = sys.argv[1]

  os.system('iqall -J$USER -jdisect-%s-%% -a10 | grep disect | sort -r -k2 -t-' % prefix)
  print
  print

  height, width = ex_ret('stty size').split()
  width = int(width)

  jobs = ic.job_list(os.getenv('USER'), 10*86400, 0, 'disect-%s-%%' % prefix)
  jobs = filter(lambda j: j['state'] > 0, jobs)
  jobs.sort(key = lambda j: j['name'])
  gitid_head = jobs[-1]['name'].split('-')[-6]
  results = {}
  for job in jobs:
    iqlib.job_annotate(job)
    if job['state'] == intelqueue.JobState.DELETED:
      continue

    sim = ic.graphite_stat(job['jobid'])
    gitid = sim['gitid']
    if gitid not in results: results[gitid] = ''
    if job['state'] == intelqueue.JobState.DONE:
      s = '+'
    elif job['state'] == intelqueue.JobState.ERROR:
      s = '-'
    else:
      s = intelqueue.JobState.shortNames[job['state']][0]
    results[gitid] += s

  tree = git('log --max-count=1000 --format="format:%H %at %s=" --graph '+gitid_head).split('\n')
  for line in tree:
    if not line.endswith('='):
      # Lines without a commit (just the merge part of the graph)
      print line
      continue
    res = re.match(r'([ *|/\\]+) ([0-9a-f]*) ([0-9]*) (.*)=', line)
    if not res:
      raise ValueError("Unparseable line %s" % line)
    tree, gitid, t, subject = res.groups()
    out = ' '.join([ '%-20s ' % tree, gitid[:12], '  ', time.strftime('%Y%m%d.%H%M%S', time.localtime(float(t))), '  ' ])
    if gitid in results:
      out += ' %-8s' % results[gitid]
      del(results[gitid])
    else:
      out += ' '*9
    out += '      '
    out += subject[:max(30, width-len(out)-1)]
    print out
    if not results:
      break
  sys.exit(0)


def usage():
  print '%s <prefix> {iqgraphite options: pnicgJq} [-N <identical copies (1)>] gitid..gitid' % sys.argv[0]
  sys.exit(-1)

if len(sys.argv) < 3:
  usage()

prefix = sys.argv[1]
jobgroup = os.getenv('USER')
bm = 'splash2-fft'
inputsize = 'test'
ncores = 1
numruns = 1
priority = 10
graphiteoptions = []
files = []
gitid_start = None
gitid_end = None

try:
  opts, args = getopt.getopt(sys.argv[2:], "hJ:q:p:n:i:N:c:g:", [])
except getopt.GetoptError, e:
  # print help information and exit:
  print e
  usage()
for o, a in opts:
  if o == '-h':
    usage()
    sys.exit()
  if o == '-J':
    jobgroup = a
  if o == '-p':
    bm = a
  if o == '-i':
    inputsize = a
  if o == '-n':
    ncores = int(a)
  if o == '-N':
    numruns = int(a)
  if o == '-q':
    priority = int(a)
  if o == '-c':
    if os.path.exists(a):
      files.append(os.path.abspath(os.path.join(a)))
      a = os.path.basename(a)
    graphiteoptions.append('-c ' + a)
  if o == '-g':
    graphiteoptions.append('-g ' + a)

try:
  gitid_start, gitid_end = args[0].split('..')
except:
  print 'Need startgitid..endgitid as argument'
  usage()


graphiteoptions = ' '.join(graphiteoptions)
if 'reschedule_cost' not in graphiteoptions:
  graphiteoptions += ' -g --perf_model/sync/reschedule_cost=%u' % {8:3000,16:4000}.get(ncores, 1000)


def startsim(jobname, gitid):
  package, program = bm.rsplit("-",1)
  try:
    program = __import__(package).Program(program, ncores, inputsize)
  except:
    raise
    return # unsupported (bm, inputsize, ncores) combination, don't start

  iqlib.graphite_submit(ic, jobgroup, jobname, priority, gitid, 'none', bm, inputsize, ncores, graphiteoptions, files, constraints = {})


gitids = list(reversed(git('log --format=format:%%H %s..%s' % (gitid_start, gitid_end)).split()))

if len(gitids) > 5:
  step = len(gitids) / 5
  gitids = [ gitid_start ] + [ gitids[i] for i in range(0, len(gitids)-1, step) ] + [ gitid_end ]
else:
  gitids = [ gitid_start ] + gitids + [ gitid_end ]

for gitid in reversed(gitids):
  gitid = git('rev-parse %s' % gitid)[:12]
  datespec = git('show --format=format:%%ai %s' % gitid)[:19].replace('-', '').replace(':', '').replace(' ', '')
  for run in range(numruns):
    jobname = 'disect-%s-%s-%s-%s' % (prefix, datespec, gitid, run+1)
    if ic.graphite_exists(jobgroup, jobname, None, bm, inputsize, ncores, graphiteoptions):
      continue
    startsim(jobname, gitid)
    print jobname
