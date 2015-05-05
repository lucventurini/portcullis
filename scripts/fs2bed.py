__author__ = 'maplesod'

import sys

with open(sys.argv[1]) as f:
    content = f.readline()

    print "track name=\"junctions\""

    index = 0;
    for line in f:
        words = line.split()

        dif = int(words[2]) - int(words[1])

        print words[0] + "\t" + str(words[1]) + "\t" + str(words[2]) + "\tjunc_" + str(index) + "\t0.0\t.\t" + str(words[1]) + "\t" + str(words[2]) + "\t255,0,0\t2\t0,0\t0," + str(dif)
        index += 1