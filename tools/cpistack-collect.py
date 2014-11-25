#!/usr/bin/env python2

import sys, os, env_setup
sys.path.append(os.path.join(env_setup.benchmarks_root(), 'tools', 'scheduler'))
import getopt, intelqueue, iqclient, iqlib, cpistack, buildstack, gnuplot

ic = iqclient.IntelClient()

age = 0 # in days, 0 == get all
use_simple = False
groupname = 'iiswc2011'
outputpath = 'cpistack-collect'
jobids = {}

errors_seen = 0
jobs_processed = 0

for job in ic.job_list(groupname, age*86400, intelqueue.JobState.DONE):
  iqlib.job_annotate(job)
  # Store the most recently completed jobid
  jobids[job['name']] = job['jobid']

# Sort the jobs
jobids_x = sorted(jobids.iteritems(), key=lambda (name, jobid): (name.split('-')[1], name.split('-')[2], name.split('-')[3], int(name.split('-')[4])))

html_file = open(os.path.join(outputpath, 'index.html'), 'w')
html_file.write(r'''
<html>
<head>
<script type="text/javascript" src="jquery.js"></script>
<script type="text/javascript">
function showDiv(objectID) {
 var theElementStyle = document.getElementById(objectID);
 if(theElementStyle.style.display == "none")
 {
  theElementStyle.style.display = "block";
 }
 else
 {
  theElementStyle.style.display = "none";
 }
}
function showAllDiv() {
 $('[id^=cpistack-]').each(function() {
  this.style.display = "block";
 });
}
function hideAllDiv() {
 $('[id^=cpistack-]').each(function() {
  this.style.display = "none";
 });
}
</script>
</head>
<body>
 <a href="#" onClick="showAllDiv();return false;">Show All</a></br>
 <a href="#" onClick="hideAllDiv();return false;">Hide All</a></br>
''')

# Start with a fresh CSV file
file(os.path.join(outputpath, 'cpi-stack.csv'), 'w').write('')

csv_print_header = True # Only print the header one time
for (jobname, jobid) in jobids_x:
  print jobname, jobid

  def output(jobname, threads):
    global csv_print_header, jobs_processed, errors_seen
    outputfile = 'cpistack-%d-%s' % ( jobid, jobname )

    try:
      cpistack.cpistack(jobid = jobid, job_name = jobname, outputfile = outputfile + '-cpi', outputdir = outputpath, use_simple_mem = False, gen_text_stack = False, use_cpi = True, threads = threads)
      cpistack.cpistack(jobid = jobid, job_name = jobname, outputfile = outputfile + '-cycle', outputdir = outputpath, use_simple_mem = False, gen_text_stack = False, use_cpi = False, threads = threads)
      cpistack.cpistack(jobid = jobid, job_name = jobname, outputfile = outputfile + '-nocollapse', outputdir = outputpath, use_simple_mem = False, gen_text_stack = False, use_cpi = False, no_collapse = True, gen_csv_stack = True, csv_print_header = csv_print_header, threads = threads)
      csv_print_header = False
      jobs_processed += 1
    except (KeyError, ZeroDivisionError), e:
      raise
      errors_seen += 1
      # For debugging
      #sys.stderr.write(str(jobid) + '\n');
      return

    html_file.write(r'''
<div>
 <a href="#" onClick="showDiv('%s');return false;">%s</a>
 <div id="%s" style='display: none'>
  <table style="width: 100%%" cellspacing="0" cellpadding="0">
   <tr>
    <td><img src="%s-cpi.jpg"/></td>
    <td><img src="%s-cycle.jpg"/></td>
    <td><img src="%s-nocollapse.jpg"/></td>
   </tr>
  </table>
 </div>
</div>
''' % (outputfile, outputfile, outputfile, outputfile, outputfile, outputfile) )

  if 'parsec-dedup' in jobname:
    nthreads = int(jobname.split('-')[-1]) / 4
    names = ['ChunkProcess', 'FindAllAnchors', 'Compress']
    ttt = [range(1,1+nthreads),range(1+nthreads,1+2*nthreads),range(1+2*nthreads,1+3*nthreads)]
    for name, threads in zip(names, ttt):
      output(jobname.replace('dedup', 'dedup_%s' % name), threads)
  elif 'parsec-ferret' in jobname:
    nthreads = (int(jobname.split('-')[-1]) - 3) / 4
    names = ['vec', 'rank']
    ttt = [range(2+2*nthreads,2+3*nthreads),range(2+3*nthreads,2+4*nthreads)]
    for name, threads in zip(names, ttt):
      output(jobname.replace('ferret', 'ferret_%s' % name), threads)
  else:
    output(jobname, None)

html_file.write(r'''
</body>
</html>
''')
html_file.close()

print '\nJobs successfully processed = %d' % jobs_processed
if errors_seen > 0:
  print 'Errors seen = %d' % errors_seen
