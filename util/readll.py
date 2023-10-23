#!/usr/bin/python

from __future__ import print_function
import sys

def printval(starty, lasty, lastval):
    global outstring, lastx
    if starty != -1:
        lastval = lastval/ 3232.0 - 467
        if lasty < 100:
            lastval = lastval - 2566
            if lasty <= 35:
                lastval = lastval - 2566
        lastval = lastval - 36 * int((lastx - 14)/2)
        if lastx >= 32:
            lastval = lastval - 28
            if lastx >= 36:
                lastval = lastval - 28
                if lastx >= 50:
                    lastval = lastval - 30
                    if lastx >= 54:
                        lastval = lastval - 28
        outstring = outstring + ' %3d-%3d/%d' % (starty, lasty, lastval)

minbit = 2201033
minx = 9999
miny = 9999
maxx = 1
maxy = 1
print('hi', sys.argv[1])
lines =  open(sys.argv[1]).readlines()
print('len', len(lines))
i = 0
toplist = {}
topoffset = {}
topref = {}
for thisline in lines:
    if thisline[0] == ';':
        continue
    iteml = thisline.split()
    if iteml[0] != 'Bit':
        print('Non-Bit line', thisline)
        continue
    if not iteml[4].startswith('Block=SLICE_X'):
        print('Non-"Block=SLICE_X" line', thisline)
        continue
    iteml.pop(0)
    for i in range(3):
        iteml[i] = int(iteml[i], 0)
    bitoff = iteml[0]
    frameoffset = iteml[2]
    temp = iteml[3][13:]
    ind = temp.find('Y')
    coordx = int(temp[:ind])
    coordy = int(temp[ind+1:])
    adjustment = 0
    itemtype = iteml[4]
    if itemtype.startswith('Ram='):
        continue
    if itemtype == 'Latch=AMUX':
        adjustment = 1
    elif itemtype == 'Latch=BMUX':
        adjustment = 19
    elif itemtype == 'Latch=BQ':
        adjustment = 25
    elif itemtype == 'Latch=CMUX':
        adjustment = 38
    elif itemtype == 'Latch=CQ':
        adjustment = 30
    elif itemtype == 'Latch=DMUX':
        adjustment = 48
    elif itemtype == 'Latch=DQ':
        adjustment = 55
    if not itemtype.endswith('MUX'):
        itemtype = f'{itemtype[:6]}   {itemtype[6:]}'
    if not topoffset.get(itemtype):
        topoffset[itemtype] = {}
    if not topoffset[itemtype].get(frameoffset):
        topoffset[itemtype][frameoffset] = 0
    topoffset[itemtype][frameoffset] = topoffset[itemtype][frameoffset] + 1
    ftemp = frameoffset % 32
    if not topref.get(ftemp):
        topref[ftemp] = []
    if itemtype not in topref[ftemp]:
        topref[ftemp].append(itemtype)
    adjoff = bitoff - coordx - 64 * coordy - adjustment
    maxx = max(maxx, coordx)
    maxy = max(maxy, coordy)
    minx = min(minx, coordx)
    miny = min(miny, coordy)
    #print('index', '%4d_%4d_%5d' % (coordy, coordx, frameoffset))
    toplist['%4d_%4d_%5d' % (coordx, coordy, frameoffset)] = [ coordx, coordy, bitoff - frameoffset]
print('min', minx, miny, 'max', maxx, maxy)
lastx = 0
outstring = ''
starty = -1
lasty = -1
lastval = -1
for key, value in sorted(toplist.items()):
    if value[0] != lastx:
        printval(starty, lasty, lastval)
        lastx = value[0]
        print(outstring)
        outstring = '%3d:' % value[0]
        starty = -1
        lasty = -1
        lastval = -1
    if lastval != value[2]:
        printval(starty, lasty, lastval)
        starty = value[1]
    lasty = value[1]
    lastval = value[2]
printval(starty, lasty, lastval)
print(outstring)
for key, value in sorted(topoffset.items()):
    outstring = f'{key}: '
    for vkey, vvalue in sorted(value.items()):
        if vvalue != 1:
            outstring = f'{outstring} {str(vkey)}/{str(vvalue)}'
    print(outstring)
for key, value in sorted(topref.items()):
    print('ref', key, value)

