#pragma once
// Number helpers the model's maths and texts share. Values shown to the user
// round half to even ("12.5 %" shows as "12 %"), as they always have.

#include <QString>

namespace sub::app {

// The nearest whole number, halves to the even one (2.5 -> 2, 3.5 -> 4).
double roundHalfEven(double value);

// a // b for floating-point values: the floor of the true quotient, exact
// where floor(a / b) would round up (1 // 0.1 is 9).
double floorDiv(double a, double b);

// a % b as Python has it for whole numbers (b > 0): 0..b-1, also for a < 0 (-1 % 12 is 11).
inline int floorMod(int a, int b) { return ((a % b) + b) % b; }

// `value` with `decimals` digits after the point, rounded half to even on its
// exact binary value; with `sign`, positive values get a "+".
QString formatFixed(double value, int decimals, bool sign = false);

}  // namespace sub::app
