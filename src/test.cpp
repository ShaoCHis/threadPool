#include "threadPool.hpp"

int sum1(int a,int b)
{
    //比较耗时
    return a+b;
}

int sum2(int a,int b,int c)
{
    return a+b+c;
}


int main()
{

    ThreadPool2 pool;
    pool.setMode(PoolMode::MODE_CACHED);
    pool.start(3);
    std::this_thread::sleep_for(std::chrono::seconds(4));
    std::future<int> r1 = pool.submitTask(sum1,1,2);
    std::cout << r1.get() << std::endl;
    //这里Result对象要析构
    /**
     * 在vs下，条件变量析构会释放相关资源
     */
    std::this_thread::sleep_for(std::chrono::seconds(4));
    std::cout << "main Over!!!" << std::endl;
}

