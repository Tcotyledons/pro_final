#include <stdio.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>

int main()
{
    //printf("Content-type:text/html\n\n");
   // char MESSAGE[1024]="Content-Type:text/html; charset=utf-8\r\n\r\n学号：202234261021\t姓名：洪育懋\t时间：";
    //得到当前时间
    char str[30] = {0}; 
    time_t clock;
    time(&clock); 
    strcpy(str, ctime(&clock)); //将time_t类型的结构体中的时间，按照一定格式保存成字符串，
   // strcat(MESSAGE,str);
    printf("Content-Type:text/html; charset=utf-8\r\n\r\n学号：202234261021\t姓名：洪育懋\t时间：%s",str);
    return 0;
}