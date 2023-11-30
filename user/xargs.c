#include "../kernel/types.h"
#include "../user/user.h"
#include "../kernel/param.h"

int main(int argc,char *argv[]){
    char *xargv[MAXARG];
    char buf[512];
    for (int i = 1; i < argc; i++){
        xargv[i-1]=argv[i];
    }
    while (1){
        int readid=0;
        int readlen=0;
        while (1){
            readlen=read(0,&buf[readid],sizeof(char));
            if (readlen==0||buf[readid]=='\n'){
                break;
            }
            readid++;
        }
        if (readlen==0){
            break;
        }
        buf[readid]='\0';
        xargv[argc-1]=buf;
        if (fork()==0){
            exec(xargv[0],xargv);
            exit(0);
        }else{
            wait(0);
        }
    }
    exit(0);
}

