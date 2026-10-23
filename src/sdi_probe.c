/* TEMPORARY — delete after the wlink sdi-print experiment.
 * Prints continuously over ch32fun's SWIO debug channel so the host side can
 * be tested without racing a boot banner. */
#include "ch32fun.h"
#include <stdint.h>

int main(void)
{
    SystemInit();
    for (uint32_t i = 0;; i++) {
        printf("sdi probe %lu\n", (unsigned long)i);
        Delay_Ms(500);
    }
}
