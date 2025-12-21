// client.c
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

int main(int argc,char *argv[]) {
    if(argc!=2) return 1;
    int id = atoi(argv[1]);

    char fifo[32];
    sprintf(fifo, "p%d.fifo", id);
    int fd = open(fifo, O_WRONLY);

    while (1) {
        int r,c;
        printf("Player %d enter row col: ", id);
        scanf("%d %d",&r,&c);
        write(fd,&r,sizeof(int));
        write(fd,&c,sizeof(int));
    }
}
