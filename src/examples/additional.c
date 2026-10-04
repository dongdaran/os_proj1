#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <syscall.h>

// 3-5 : 부호와 숫자를 검사하고, 곱셈 전에 int 범위 초과를 거부한다.
static bool
parse_int (const char *text, int *result)
{
  bool negative = *text == '-';
  unsigned value = 0;
  unsigned limit = negative ? (unsigned) INT_MAX + 1 : INT_MAX;
  if (*text == '-' || *text == '+')
    text++;
  if (*text == '\0')
    return false;
  for (; *text != '\0'; text++)
    {
      unsigned digit;
      if (*text < '0' || *text > '9')
        return false;
      digit = *text - '0';
      if (value > (limit - digit) / 10)
        return false;
      value = value * 10 + digit;
    }
  *result = negative ? (value == (unsigned) INT_MAX + 1
                        ? INT_MIN : -(int) value) : (int) value;
  return true;
}

int
main (int argc, char **argv)
{
  int values[4], i;
  if (argc != 5)
    {
      printf ("usage: additional INT INT INT INT\n");
      return EXIT_FAILURE;
    }
  for (i = 0; i < 4; i++)
    if (!parse_int (argv[i + 1], &values[i]))
      {
        printf ("additional: invalid integer\n");
        return EXIT_FAILURE;
      }
  // 3-5 : 사용자 측에서 계산하지 않고 두 시스템 콜을 실제 호출한다.
  printf ("%d %d\n", fibonacci (values[0]),
          max_of_four_int (values[0], values[1], values[2], values[3]));
  return EXIT_SUCCESS;
}
