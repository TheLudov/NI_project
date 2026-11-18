/* autotune/main.c: chooses parameters for your LB on this machine (a stub: it chooses none).
 *
 *   build/autotune
 *
 * takes no argument and prints exactly one line on standard output: the
 * parameters, key=value pairs separated by commas (an empty line for none),
 * and nothing else; diagnostics go to standard error.  It may measure your
 * components (in src/) with gen_open in bench mode (mode=bench, and a results
 * path of your own: results=/dev/null to keep none).  The contest runs
 *
 *   build/lb mixed_flood.scn "$(build/autotune),$(cat config.params)"
 */
#include <stdio.h>

int main(void)
{
    printf("\n");
    return 0;
}
