#pragma once
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Pocket calculator: immediate (left-to-right) evaluation, no operator precedence.
struct UtilityCalculator {
  char display[32] = "0";
  double operand = 0;
  char pending = 0;
  bool fresh = true;
  bool error = false;

  void clear() { strcpy(display, "0"); operand = 0; pending = 0; fresh = true; error = false; }
  void key(char k) {
    if (k == 'C') { clear(); return; }
    if (error) return;
    if ((k >= '0' && k <= '9') || k == '.') {
      if (fresh) { strcpy(display, "0"); fresh = false; }
      if (k == '.' && strchr(display, '.')) return;
      size_t n = strlen(display);
      if (n >= 15) return;
      if (k != '.' && !strcmp(display, "0")) { display[0] = k; display[1] = 0; }
      else { display[n] = k; display[n + 1] = 0; }
      return;
    }
    if (k != '+' && k != '-' && k != '*' && k != '/' && k != '=') return;
    double value = strtod(display, nullptr);
    if (pending && !fresh) {
      if (pending == '+') value = operand + value;
      else if (pending == '-') value = operand - value;
      else if (pending == '*') value = operand * value;
      else if (pending == '/') {
        if (value == 0) { strcpy(display, "Error"); error = true; return; }
        value = operand / value;
      }
      if (!isfinite(value) || fabs(value) > 1e15) { strcpy(display, "Error"); error = true; return; }
      snprintf(display, sizeof(display), "%.9g", value);
    }
    operand = value;
    pending = k == '=' ? 0 : k;
    fresh = true;
  }
};
