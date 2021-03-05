#include "util.h"
#include <stdlib.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    LaunchMode lm = load_arguments(argc, argv);
    switch (lm)
    {
        case profiler:
            printf("Detected profiler mode\n");
            break;
        case syscall:
            printf("Detected syscall mode\n");
            break;
        case error:
            fprintf(stderr, "Error while parsing arguments!\n");
            return -1;
    
        default:
            fprintf(stderr, "Invalid arguments!\n");
            return -1;
    }

    return EXIT_SUCCESS;
}
