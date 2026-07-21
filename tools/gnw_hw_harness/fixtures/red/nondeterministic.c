#include <stdio.h>
#include <time.h>
#include <unistd.h>

int main(void)
{
    printf("RUNHASH=%ld-%ld\n", (long)time(NULL), (long)getpid());
    return 0;
}
