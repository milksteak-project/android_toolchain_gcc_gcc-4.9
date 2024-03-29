// 2001-04-06 gdr

// Copyright (C) 2001-2014 Free Software Foundation, Inc.
//
// This file is part of the GNU ISO C++ Library.  This library is free
// software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the
// Free Software Foundation; either version 3, or (at your option)
// any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License along
// with this library; see the file COPYING3.  If not see
// <http://www.gnu.org/licenses/>.


// { dg-do compile { xfail uclibc } }
// { dg-excess-errors "" { target uclibc } }

#include <cmath>

void fpclassify() { }

void isfinite() { }

void isinf() { }

void isnan() { }

void isnormal() { }

void signbit() { }

void isgreater() { }

void isgreaterequal() { }

void isless() { }

void islessequal() { }

void islessgreater() { }

void isunordered() { }

#if _GLIBCXX_USE_C99_MATH
template <typename _Tp>
  void test_c99_classify()
  {
    bool test __attribute__((unused)) = true;

    typedef _Tp fp_type;
    fp_type f1 = 1.0;
    fp_type f2 = 3.0;
    int res = 0;
    
    res = ::fpclassify(f1);
    res = ::isfinite(f2);
    res = ::isinf(f1);
    res = ::isnan(f2);
    res = ::isnormal(f1);
    res = ::signbit(f2);
    res = ::isgreater(f1, f2);
    res = ::isgreaterequal(f1, f2);
    res = ::isless(f1, f2);
    res = ::islessequal(f1,f2);
    res = ::islessgreater(f1, f2);
    res = ::isunordered(f1, f2);
    res = res; // Suppress unused warning.
  }
#endif

int main()
{
#if _GLIBCXX_USE_C99_MATH
  test_c99_classify<float>();
  test_c99_classify<double>();
#endif
  return 0;
}
