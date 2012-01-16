#!/usr/bin/python

import os
import re
import sys

def print_data(data):

    print "{",

    first = True

    for (t, c) in data:

        if not first:
            print ",",

        first = False
        
        print "{ %s, %s }" % (t, c),

    print "}",

def get_data(f):

    pattern = re.compile(r"time: (\d+), cycles: (\d+)")

    data = []

    while True:

        s = f.readline()

        if s == "":
            break

        m = pattern.match(s)

        if m == None:
            print >> sys.stderr, "No match!", s
            continue

        data.append(m.group(1,2))

    return data

def main():

    i = 0

    data = []

    # gather data
    while True:

        try:
            f = open("progress_trace_%d" % i)

            data.append(get_data(f))

        except IOError:
            break

        i += 1

    # print data
    print "data = Table[ {}, {%d} ];" % i
    
    for j in range(i):
        
        print "data[[%d]] =" % (j+1),

        print_data(data[j])

        print ";"

if __name__ == "__main__":
    sys.exit(main())
