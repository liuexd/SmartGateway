#include <stdio.h>
#include "ota_verify.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr,
                "Usage: %s <package-directory>\n",
                argv[0]);
        return 1;
    }

    return ota_verify_package(argv[1]);
}