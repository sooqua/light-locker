#include "shell.h"
#include <stdio.h>

int run_script(gchar* path)
{
    gchar line[512];
    int linenr;
    FILE *pipe;

    pipe = popen(path, "r");
    if (pipe == NULL)
    {
        perror(NULL);
        return 1;
    }

    for (linenr = 1; fgets(line, sizeof line, pipe) != NULL; ++linenr) {
        printf("%s", line);
    }

    pclose(pipe);
    return 0;
}
