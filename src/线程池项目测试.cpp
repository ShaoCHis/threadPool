#include "threadpool.h"
#include <iostream>

#define uLong unsigned long long

/**
有些场景，是希望能够获取线程执行任务的返回值
举例：
1 + 。。。。+ 30000 的和，多核核心通过多线程来进行计算结果
 */

class MyTask : public Task
{
public:
    MyTask(int begin, int end)
        : begin_(begin), end_(end)
    {
    }

    // 问题一：怎么设计run函数的返回值，可以表示任意的类型
    // Java Python  Object 是所有其他类类型的基类
    // C++17 Any类型
    Any run() override // run方法最终就在线程池分配的线程中去做执行了
    {
        std::cout << "tid:" << std::this_thread::get_id() << " begin!" << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(4));
        uLong sum = 0;
        for (int i = begin_; i <= end_; i++)
        {
            /* code */
            sum += i;
        }

        std::cout << "tid:" << std::this_thread::get_id() << " end!" << std::endl;
        return sum;
    }

private:
    int begin_;
    int end_;
};

int main()
{
    {
        ThreadPool pool;
        pool.setMode(PoolMode::MODE_CACHED);
        pool.start(2);
        // linux上，这些result对象也是局部对象，也会析构
        std::shared_ptr<Result> res1 = pool.submitTask(std::make_shared<MyTask>(1, 100000000));
        std::shared_ptr<Result> res2 = pool.submitTask(std::make_shared<MyTask>(1, 100000000));
        pool.submitTask(std::make_shared<MyTask>(1, 100000000));
        pool.submitTask(std::make_shared<MyTask>(1, 100000000));
        pool.submitTask(std::make_shared<MyTask>(1, 100000000));
        std::cout <<res1.get()->get().cast_<uLong>();
    }   //这里Result对象要析构
    /**
     * 在vs下，条件变量析构会释放相关资源
     */
    std::cout << "main Over!!!" << std::endl;
#if 0
    //问题 ThreadPool对象析构后，怎么样把线程池相关的线程资源全部回收
    {
        ThreadPool pool;
        //用户可以自己设置线程池的工作模式
        pool.setMode(PoolMode::MODE_CACHED);
        pool.start(3);

        

        // 如何设计这里的 Result 机制
        std::shared_ptr<Result> res1 = pool.submitTask(std::make_shared<MyTask>(1,1000));
        std::shared_ptr<Result> res2 = pool.submitTask(std::make_shared<MyTask>(1001,2000));
        std::shared_ptr<Result> res3 = pool.submitTask(std::make_shared<MyTask>(2001,3000));
        std::shared_ptr<Result> res4 = pool.submitTask(std::make_shared<MyTask>(3001,4000));
        std::shared_ptr<Result> res5 = pool.submitTask(std::make_shared<MyTask>(4001,5000));
        std::shared_ptr<Result> res6 = pool.submitTask(std::make_shared<MyTask>(5001,6000));
        //随着task被执行完，task对象没了，依赖于task对象的Result对象也没了
        int sum = res1.get()->get().cast_<int>()+res2.get()->get().cast_<int>()+res3.get()->get().cast_<int>();
        //Master -Slave线程模型
        //Master线程用来分解任务，然后给各个Salve线程分配任务
        //等待各个Slave线程执行完任务，返回结果
        //Master线程合并各个任务结果，输出
        std::cout << sum << std::endl;
    }
    getchar();
#endif
    return 0;
}