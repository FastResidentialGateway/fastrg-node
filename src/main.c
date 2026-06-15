#include <stdio.h>

#include <common.h>

#include "fastrg.h"
#include "utils.h"

int main(int argc, char **argv)
{
    /* Handle -h/--help before anything else so it works regardless of how many
     * other args are given (and before EAL would reject an unknown -h). */
    if (fastrg_help_requested(argc, argv)) {
        fastrg_print_usage(argv[0], stdout);
        return SUCCESS;
    }

    return fastrg_start(argc, argv);
}
