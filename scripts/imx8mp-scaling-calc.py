#!/usr/bin/env python3

from fractions import Fraction
import sys

def print_usage(name):
    print(f'Usage: {name} <input size> <crop window size> <output size>')
    print('    All sizes are in the format: WxH')

def produce_output(oversize, overscale):
    output = 'The scaling will '
    if oversize or overscale:
        output += 'not '
    output += 'work'
    if oversize or overscale:
        output += ', as '
    if oversize:
        output += 'the output of upscaling is too big (over 3MP)'
    if oversize and overscale:
        output += ', and '
    if overscale:
        output += 'the upscaling that the resizer needs to do is greater than x2'

    return output

class Size(object):
    def __init__(self, w, h):
        self.w = w
        self.h = h

    def __init__(self, s):
        self.w, self.h = map(lambda x: int(x), s.split('x'))

    def aspect_ratio(self):
        return Fraction(self.w, self.h)

    def scale_factor_to(self, other):
        return (other.w/self.w, other.h/self.h)

def main(argv):
    if len(argv) != 4:
        print_usage(argv[0])
        exit(1)

    in_size = Size(argv[1])
    crop_size = Size(argv[2])
    out_size = Size(argv[3])

    eff_scale = 1/max(in_size.scale_factor_to(crop_size))
    print(f'Effective scaling: x{eff_scale}')

    rsz_scale = crop_size.scale_factor_to(out_size)
    print(f'Resizer scaling: {rsz_scale}')

    oversize = out_size.w * out_size.h > 2240 * 1260 and max(rsz_scale) > 1
    overscale = max(rsz_scale) > 2

    print(produce_output(oversize, overscale))

if __name__ == '__main__':
    main(sys.argv)
