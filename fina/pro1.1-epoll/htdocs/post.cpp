#include <iostream>
#include <time.h>
#include <string.h>

using namespace std;


int main()
{
    char ch;
    char *p = getenv("CONTENT_LENGTH");/*从环境变量CONTENT_LENGTH中得到数据长度*/
    int len = atoi(p);
    int i=0;
    setvbuf(stdin,NULL,_IONBF,0);     /*关闭stdin的缓冲*/
    cout<<"Content-Type:text/html; charset=utf-8\r\n\r\n";
    cout<<"这就是post表单的内容"<<endl;
    while(i<len){
        ch = fgetc(stdin);
        cout<<ch;
        i++;
    }
}