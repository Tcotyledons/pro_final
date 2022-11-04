#include <iostream>
#include <time.h>
#include <string.h>

using namespace std;


int main()
{
    time_t clock;
    time(&clock);
    cout<<"Content-Type:text/html; charset=utf-8\r\n\r\n";
    cout<<"学号:202234061001 姓名:谭子豪 "<<ctime(&clock);
}