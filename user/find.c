#include "../kernel/types.h"
#include "../kernel/stat.h"
#include "../user/user.h"
#include "../kernel/fs.h"

void ls(char *directory,char *filename){
	char buf[512], *p;
	int fd;
	struct dirent de;
	struct stat st;

	if((fd = open(directory, 0)) < 0){
		fprintf(2, "ls: cannot open %s\n", directory);
		return;
	}

	if(fstat(fd, &st) < 0){
		fprintf(2, "ls: cannot stat %s\n", directory);
		close(fd);
		return;
	}

	switch(st.type){
	case T_FILE:
		fprintf(2,"ls: cannot get there\n");
		exit(1);

	case T_DIR:
		if(strlen(directory) + 1 + DIRSIZ + 1 > sizeof buf){
			printf("ls: path too long\n");
			break;
		}
		strcpy(buf, directory);
		p = buf+strlen(buf);
		*p++ = '/';
		while(read(fd, &de, sizeof(de)) == sizeof(de)){
			if(de.inum == 0||strcmp(de.name,".")==0||strcmp(de.name,"..")==0)
				continue;
			memmove(p, de.name, DIRSIZ);
			p[DIRSIZ] = 0;
			
			if(stat(buf, &st) < 0){
				printf("ls: cannot stat %s\n", buf);
				continue;
			}
		
			if (st.type==T_FILE){
				if (strcmp(de.name,filename)==0){
					printf("%s\n",buf);
				}
				
			}else{
				ls(buf,filename);
			}
		}
		break;
	}
	close(fd);
}

int main(int argc, char *argv[]){
    if(argc !=3){
        fprintf(2, "usage: find [directory] [target filename]\n");
        exit(1);
    }
    ls(argv[1],argv[2]);
    exit(0);
}
