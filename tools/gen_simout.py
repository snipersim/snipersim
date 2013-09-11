#!/usr/bin/env python

import sys, sniper_lib


def generate_simout(jobid = None, resultsdir = None, output = sys.stdout, silent = False):

  try:
    res = sniper_lib.get_results(jobid = jobid, resultsdir = resultsdir)
  except (KeyError, ValueError), e:
    if not silent:
      print 'Failed to generated sim.out:', e
    return

  results = res['results']
  config = res['config']
  ncores = int(config['general/total_cores'])

  format_int = lambda v: str(long(v))
  def format_ns(digits):
    return lambda v: ('%%.%uf' % digits) % (v/1e6)

  time0_begin = max(results['performance_model.elapsed_time_begin'])
  time0_end = max(results['performance_model.elapsed_time_end'])

  if 'barrier.global_time' in results:
    time0 = results['barrier.global_time'][0]
  else:
    time0 = time0_begin - time0_end

  if sum(results['performance_model.instruction_count']) == 0:
    # core.instructions is less exact, but in cache-only mode it's all there is
    results['performance_model.instruction_count'] = results['core.instructions']

  results['performance_model.elapsed_time_fixed'] = [
    results['performance_model.elapsed_time_end'][c] - time0_begin
    for c in range(ncores)
  ]
  results['performance_model.cycle_count_fixed'] = [
    results['performance_model.elapsed_time_fixed'][c] * results['fs_to_cycles_cores'][c]
    for c in range(ncores)
  ]

  template = [
    ('  Instructions', 'performance_model.instruction_count', str),
    ('  Cycles',       'performance_model.cycle_count_fixed', format_int),
    ('  Time (ns)',    'performance_model.elapsed_time_fixed', format_ns(0)),
  ]

  if 'branch_predictor.num-incorrect' in results:
    results['branch_predictor.missrate'] = [ 100 * float(results['branch_predictor.num-incorrect'][core])
      / ((results['branch_predictor.num-correct'][core] + results['branch_predictor.num-incorrect'][core]) or 1) for core in range(ncores) ]
    results['branch_predictor.mpki'] = [ 1000 * float(results['branch_predictor.num-incorrect'][core])
      / (results['performance_model.instruction_count'][core] or 1) for core in range(ncores) ]
    template += [
      ('Branch predictor stats', '', ''),
      ('  num correct',  'branch_predictor.num-correct', str),
      ('  num incorrect','branch_predictor.num-incorrect', str),
      ('  misprediction rate', 'branch_predictor.missrate', lambda v: '%.2f%%' % v),
      ('  mpki', 'branch_predictor.mpki', lambda v: '%.2f' % v),
    ]

  template += [
    ('TLB Summary', '', ''),
  ]

  for tlb in ('itlb', 'dtlb', 'stlb'):
    if '%s.access'%tlb in results:
      results['%s.missrate'%tlb] = map(lambda (a,b): 100*a/float(b or 1), zip(results['%s.miss'%tlb], results['%s.access'%tlb]))
      results['%s.mpki'%tlb] = map(lambda (a,b): 1000*a/float(b or 1), zip(results['%s.miss'%tlb], results['performance_model.instruction_count']))
      template.extend([
        ('  %s' % {'itlb': 'I-TLB', 'dtlb': 'D-TLB', 'stlb': 'L2 TLB'}[tlb], '', ''),
        ('    num accesses', '%s.access'%tlb, str),
        ('    num misses', '%s.miss'%tlb, str),
        ('    miss rate', '%s.missrate'%tlb, lambda v: '%.2f%%' % v),
        ('    mpki', '%s.mpki'%tlb, lambda v: '%.2f' % v),
      ])

  template += [
    ('Cache Summary', '', ''),
  ]

  allcaches = [ 'L1-I', 'L1-D' ] + [ 'L%u'%l for l in range(2, 5) ]
  existcaches = [ c for c in allcaches if '%s.loads'%c in results ]
  for c in existcaches:
    results['%s.accesses'%c] = map(sum, zip(results['%s.loads'%c], results['%s.stores'%c]))
    results['%s.misses'%c] = map(sum, zip(results['%s.load-misses'%c], results['%s.store-misses-I'%c]))
    results['%s.missrate'%c] = map(lambda (a,b): 100*a/float(b) if b else float('inf'), zip(results['%s.misses'%c], results['%s.accesses'%c]))
    results['%s.mpki'%c] = map(lambda (a,b): 1000*a/float(b) if b else float('inf'), zip(results['%s.misses'%c], results['performance_model.instruction_count']))
    template.extend([
      ('  Cache %s'%c, '', ''),
      ('    num cache accesses', '%s.accesses'%c, str),
      ('    num cache misses', '%s.misses'%c, str),
      ('    miss rate', '%s.missrate'%c, lambda v: '%.2f%%' % v),
      ('    mpki', '%s.mpki'%c, lambda v: '%.2f' % v),
    ])

  allcaches = [ 'nuca-cache', 'dram-cache' ]
  existcaches = [ c for c in allcaches if '%s.reads'%c in results ]
  for c in existcaches:
    results['%s.accesses'%c] = map(sum, zip(results['%s.reads'%c], results['%s.writes'%c]))
    results['%s.misses'%c] = map(sum, zip(results['%s.read-misses'%c], results['%s.write-misses'%c]))
    results['%s.missrate'%c] = map(lambda (a,b): 100*a/float(b) if b else float('inf'), zip(results['%s.misses'%c], results['%s.accesses'%c]))
    results['%s.mpki'%c] = map(lambda (a,b): 1000*a/float(b) if b else float('inf'), zip(results['%s.misses'%c], results['performance_model.instruction_count']))
    template.extend([
      ('  %s cache'% c.split('-')[0].upper(), '', ''),
      ('    num cache accesses', '%s.accesses'%c, str),
      ('    num cache misses', '%s.misses'%c, str),
      ('    miss rate', '%s.missrate'%c, lambda v: '%.2f%%' % v),
      ('    mpki', '%s.mpki'%c, lambda v: '%.2f' % v),
    ])

  results['dram.accesses'] = map(sum, zip(results['dram.reads'], results['dram.writes']))
  results['dram.avglatency'] = map(lambda (a,b): a/b if b else float('inf'), zip(results['dram.total-access-latency'], results['dram.accesses']))
  template += [
    ('DRAM summary', '', ''),
    ('  num dram accesses', 'dram.accesses', str),
    ('  average dram access latency (ns)', 'dram.avglatency', format_ns(2)),
  ]
  if 'dram.total-read-queueing-delay' in results:
    results['dram.avgqueueread'] = map(lambda (a,b): a/(b or 1), zip(results['dram.total-read-queueing-delay'], results['dram.reads']))
    results['dram.avgqueuewrite'] = map(lambda (a,b): a/(b or 1), zip(results['dram.total-write-queueing-delay'], results['dram.writes']))
    template.append(('  average dram read queueing delay', 'dram.avgqueueread', format_ns(2)))
    template.append(('  average dram write queueing delay', 'dram.avgqueuewrite', format_ns(2)))
  else:
    results['dram.avgqueue'] = map(lambda (a,b): a/(b or 1), zip(results['dram.total-queueing-delay'], results['dram.accesses']))
    template.append(('  average dram queueing delay', 'dram.avgqueue', format_ns(2)))
  if 'dram-queue.total-time-used' in results:
    results['dram.bandwidth'] = map(lambda a: 100*a/time0 if time0 else float('inf'), results['dram-queue.total-time-used'])
    template.append(('  average dram bandwidth utilization', 'dram.bandwidth', lambda v: '%.2f%%' % v))


  lines = []
  lines.append([''] + [ 'Core %u' % i for i in range(ncores) ])

  for title, name, func in template:
    line = [ title ]
    if name and name in results:
      for core in range(ncores):
        line.append(' '+func(results[name][core]))
    else:
      line += [''] * ncores
    lines.append(line)


  widths = [ max(10, max([ len(l[i]) for l in lines ])) for i in range(len(lines[0])) ]
  for j, line in enumerate(lines):
    output.write(' | '.join([ ('%%%s%us' % ((j==0 or i==0) and '-' or '', widths[i])) % line[i] for i in range(len(line)) ]) + '\n')



if __name__ == '__main__':
  if len(sys.argv) > 2 and sys.argv[1] == '-j':
    generate_simout(jobid = int(sys.argv[2]))
    sys.exit(0)
  if len(sys.argv) > 1:
    resultsdir = sys.argv[1]
  else:
    resultsdir = '.'
  generate_simout(resultsdir = resultsdir)
