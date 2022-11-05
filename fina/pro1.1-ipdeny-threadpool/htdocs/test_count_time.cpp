#include <iostream>
#include<vector>
#include <algorithm>
#include <ctime>
using namespace std;
int main()
{
    
    clock_t begin, end;
    begin = clock();
    for (int i = 1; i <= 100; ++i)
    {
       
    }
    end = clock();
    cout << "100次循环所用时间：" << double(end - begin) / CLOCKS_PER_SEC * 1000 << "ms" << endl;
       
    return 0;
}
