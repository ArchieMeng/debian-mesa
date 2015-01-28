#! /usr/bin/env python
#
# Copyright (C) 2014 Intel Corporation
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice (including the next
# paragraph) shall be included in all copies or substantial portions of the
# Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.
#
# Authors:
#    Jason Ekstrand (jason@jlekstrand.net)

import nir_algebraic

# Convenience variables
a = 'a'
b = 'b'
c = 'c'
d = 'd'

# Written in the form (<search>, <replace>) where <search> is an expression
# and <replace> is either an expression or a value.  An expression is
# defined as a tuple of the form (<op>, <src0>, <src1>, <src2>, <src3>)
# where each source is either an expression or a value.  A value can be
# either a numeric constant or a string representing a variable name.  For
# constants, you have to be careful to make sure that it is the right type
# because python is unaware of the source and destination types of the
# opcodes.

optimizations = [
   (('fneg', ('fneg', a)), a),
   (('ineg', ('ineg', a)), a),
   (('fabs', ('fabs', a)), ('fabs', a)),
   (('fabs', ('fneg', a)), ('fabs', a)),
   (('iabs', ('iabs', a)), ('iabs', a)),
   (('iabs', ('ineg', a)), ('iabs', a)),
   (('fadd', a, 0.0), a),
   (('iadd', a, 0), a),
   (('fmul', a, 0.0), 0.0),
   (('imul', a, 0), 0),
   (('fmul', a, 1.0), a),
   (('imul', a, 1), a),
   (('fmul', a, -1.0), ('fneg', a)),
   (('imul', a, -1), ('ineg', a)),
   (('ffma', 0.0, a, b), b),
   (('ffma', a, 0.0, b), b),
   (('ffma', a, b, 0.0), ('fmul', a, b)),
   (('ffma', a, 1.0, b), ('fadd', a, b)),
   (('ffma', 1.0, a, b), ('fadd', a, b)),
   (('flrp', a, b, 0.0), a),
   (('flrp', a, b, 1.0), b),
   (('flrp', a, a, b), a),
   (('flrp', 0.0, a, b), ('fmul', a, b)),
   (('fadd', ('fmul', a, b), c), ('ffma', a, b, c)),
   # Comparison simplifications
   (('inot', ('flt', a, b)), ('fge', a, b)),
   (('inot', ('fge', a, b)), ('flt', a, b)),
   (('inot', ('ilt', a, b)), ('ige', a, b)),
   (('inot', ('ige', a, b)), ('ilt', a, b)),
   (('flt', ('fadd', a, b), 0.0), ('flt', a, ('fneg', b))),
   (('fge', ('fadd', a, b), 0.0), ('fge', a, ('fneg', b))),
   (('feq', ('fadd', a, b), 0.0), ('feq', a, ('fneg', b))),
   (('fne', ('fadd', a, b), 0.0), ('fne', a, ('fneg', b))),
   (('fge', ('fneg', ('fabs', a)), 0.0), ('feq', a, 0.0)),
   (('fmin', ('fmax', a, 1.0), 0.0), ('fsat', a)),
   # Logical and bit operations
   (('fand', a, 0.0), 0.0),
   (('iand', a, a), a),
   (('iand', a, 0), 0),
   (('ior', a, a), a),
   (('ior', a, 0), a),
   (('fxor', a, a), 0.0),
   (('ixor', a, a), 0),
   (('inot', ('inot', a)), a),
   # DeMorgan's Laws
   (('iand', ('inot', a), ('inot', b)), ('inot', ('ior',  a, b))),
   (('ior',  ('inot', a), ('inot', b)), ('inot', ('iand', a, b))),
   # Shift optimizations
   (('ishl', 0, a), 0),
   (('ishl', a, 0), a),
   (('ishr', 0, a), 0),
   (('ishr', a, 0), a),
   (('ushr', 0, a), 0),
   (('ushr', a, 0), 0),
   # Exponential/logarithmic identities
   (('fexp2', ('flog2', a)), a), # 2^lg2(a) = a
   (('fexp',  ('flog',  a)), a), # e^ln(a)  = a
   (('flog2', ('fexp2', a)), a), # lg2(2^a) = a
   (('flog',  ('fexp',  a)), a), # ln(e^a)  = a
   (('fexp2', ('fmul', ('flog2', a), b)), ('fpow', a, b)), # 2^(lg2(a)*b) = a^b
   (('fexp',  ('fmul', ('flog', a), b)),  ('fpow', a, b)), # e^(ln(a)*b) = a^b
   (('fpow', a, 1.0), a),
   (('fpow', a, 2.0), ('fmul', a, a)),
   (('fpow', 2.0, a), ('fexp2', a)),
   # Division and reciprocal
   (('fdiv', 1.0, a), ('frcp', a)),
   (('frcp', ('frcp', a)), a),
   (('frcp', ('fsqrt', a)), ('frsq', a)),
   (('frcp', ('frsq', a)), ('fsqrt', a)),

# This one may not be exact
   (('feq', ('fadd', a, b), 0.0), ('feq', a, ('fneg', b))),
]

print nir_algebraic.AlgebraicPass("nir_opt_algebraic", optimizations).render()
