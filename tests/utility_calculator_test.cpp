#include "../arduino/08_LVGL_Clock/utility_calculator.h"
#include <assert.h>

void input(UtilityCalculator &c, const char *keys) { while (*keys) c.key(*keys++); }
int main() {
  UtilityCalculator c;
  input(c, "12+3*2="); assert(strcmp(c.display, "30") == 0);
  input(c, "C1.5+2.5="); assert(strcmp(c.display, "4") == 0);
  input(c, "C8/0="); assert(c.error && strcmp(c.display, "Error") == 0);
  input(c, "2"); assert(c.error);
  input(c, "C9-12="); assert(strcmp(c.display, "-3") == 0);
  input(c, "+5="); assert(strcmp(c.display, "2") == 0);
  input(c, "C3+-2="); assert(strcmp(c.display, "1") == 0);
  input(c, "C0..5="); assert(strcmp(c.display, "0.5") == 0);
  input(c, "C12345678901234567890"); assert(strlen(c.display) <= 15);
}
