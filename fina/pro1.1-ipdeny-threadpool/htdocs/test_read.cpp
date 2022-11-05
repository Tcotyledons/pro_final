#include <iostream>
#include <time.h>
#include <string.h>
#include <set>
using namespace std;

int main(){
    set<string> deny;
    char buf[256];
    FILE *p = fopen(".htaccess", "r");
    char temp[1024];
    fgets(temp,sizeof(temp),p);
    while(!feof(p))
    {
        printf("%s",temp);
        if(temp[0]=='d'){
            char *str = temp+10;
            int len = strlen(str);
            str[len-1]='\0';
            printf("%s",str);
            deny.insert(str);
            cout<<strcmp(str,"0.0.0.0");
        }
        fgets(temp,sizeof(temp),p);
    }
    fclose(p);
    set<string>::iterator iter;
    
     if((iter = deny.find("0.0.0.0")) != deny.end())
     {
         cout<<"find it!"<<endl;
     }
    return 0;
}