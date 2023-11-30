#include "kernel/types.h"
#include "user/user.h"

int main(int argc,char *argv[]){
    int pid;
    int p[2];
    pipe(p);

    if(fork()==0){    //children receive and send
        pid=getpid();
        char buf[2];
        if(read(p[0],buf,1)<=0){
            fprintf(2, "children read error...\n");
            exit(1);
        }
        printf("%d: received ping\n",pid);
        close(p[0]);
        if (write(p[1],buf,1)<=0){
            fprintf(2, "children write error...\n");
            exit(1);
        }
        close(p[1]);
    }else{            //parent send and receive
        pid=getpid();
        char info[2]="a";
        char buf[2];
        buf[1]=0;
        if(write(p[1],info,1)<=0){
            fprintf(2, "parent write error...\n");
            exit(1);
        }
        close(p[1]);
        wait((int*) 0);
        if(read(p[0],buf,1)<=0){
            fprintf(2, "parent read error...\n");
            exit(1);
        }
        printf("%d: received pong\n",pid);
        close(p[0]);
    }
    exit(0);
}