#include "kernel/types.h"
#include "user/user.h"

void newprocess(int p[2]){
    close(p[1]);
    int prime;
    if (read(p[0],&prime,sizeof(prime))){
        fprintf(2,"prime %d\n",prime);
        int p2[2];
        pipe(p2);
        if (fork()==0){
            close(p2[1]);
            newprocess(p2);
        }else{
            close(p2[0]);
            int i;
            while(read(p[0],&i,sizeof(i))){
                if (i%prime!=0){
                    if (write(p2[1],&i,4)!=4){
                        printf("write to pipe false");
                        exit(1);
                    }
                }
            }
            close(p2[1]);
            wait(0);
        }
    }
    exit(0);
}

int main(int argc,char *argv[]){
    int p[2];
    pipe(p);
    if (fork()==0){
        newprocess(p);
    }else{
        close(p[0]);
        fprintf(2,"prime 2\n");
        for (int  i = 3; i <= 35; i++){
            if (i%2!=0){
                if (write(p[1],&i,4)!=4){
                    printf("write to pipe false");
                    exit(1);
                }
            } 
        }
        close(p[1]);
        wait((int*) 0);
    }
    exit(0);
}
