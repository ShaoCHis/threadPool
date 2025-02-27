#ifndef THREADPOOL_H_FIXED
#define THREADPOOL_H_FIXED

#include <vector>
#include <queue>
#include <memory>
#include <atomic>
#include <iostream>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <thread>
#include <public.h>
#include <unordered_map>
#include <future>
#include <thread>
const int TASK_MAX_THRESHHOLD = 4;
const int THREAD_MAX_THRESHHOLD = 10;
const int THREAD_MAX_IDLE_TIME = 10; // 单位：秒

// 线程支持的模式
enum class PoolMode
{
    MODE_FIXED, // 固定数量的线程
    MODE_CACHED // 线程数量可动态增长
};

// 线程类型
class Thread
{
public:
    // 线程函数对象类型
    using ThreadFunc = std::function<void(int)>;

    // 线程构造
    Thread(ThreadFunc func)
        : func_(func), threadId_(generateId_++)
    {
    }

    // 线程析构
    ~Thread() = default;

    // 启动线程
    void start()
    {
        // 创建一个线程来执行一个线程函数
        std::thread t(func_, threadId_); // C++11来说，线程对象t 和线程函数func_
        t.detach();                      // 设置分离线程  pthread_detach  pthread_t设置成分离线程
        /**
         * 这里为什么要绑定 ThreadPool中的 threadFunc
         * 因为线程池创建线程，线程执行线程函数理应由线程池提供线程所需要执行的函数
         * 同时，因为所有的条件变量的互斥量在线程池对象中，线程需要访问
         */
    }

    // 获取线程id
    int getId() const
    {
        return threadId_;
    }

private:
    ThreadFunc func_;
    static int generateId_;
    int threadId_; // 保存线程id，方便后续删除线程对象
};

/*
example:
ThreadPool pool;
pool.start(4);
class MyTask : public Task
{
public:
    void run()  { //线程代码... }
}

pool.submitTask(std::make_shared<MyTask>());
 */
// 线程池类型
class ThreadPool2
{
public:
    // 线程池构造
    ThreadPool2()
        : initThreadSize_(0), taskCnt_(0), taskQueueMaxThreshHold_(TASK_MAX_THRESHHOLD), poolMode_(PoolMode::MODE_FIXED), isRunning_(false), idleThreadSize_(0), maxThreadSize_(THREAD_MAX_THRESHHOLD), curThreadSize_(0)
    {
    }

    // 线程池析构
    ~ThreadPool2()
    {
        isRunning_ = false;
        // 等待线程池里面所有的线程返回  有两种状态：阻塞 & 正在执行任务中
        std::unique_lock<std::mutex> lock(taskQueueMtx_);
        notEmpty_.notify_all();
        exitCond_.wait(lock, [&]() -> bool
                       { return threads_.size() == 0; });
    }

    // 设置线程池的工作模式
    void setMode(PoolMode mode)
    {
        if (checkRunningState())
            return;
        poolMode_ = mode;
    }

    // 开始线程池,默认大小为CPU核心数量
    void start(int initThreadSize = std::thread::hardware_concurrency())
    {
        // 修改运行状态
        isRunning_ = true;
        // 记录初始线程个数，默认为4
        initThreadSize_ = initThreadSize;
        curThreadSize_ = initThreadSize;

        // 创建线程对象
        threads_.reserve(initThreadSize_);
        for (int i = 0; i < initThreadSize_; i++)
        {
            // 创建thread线程对象的时候，把线程函数给到thread线程对象
            std::unique_ptr<Thread> ptr(new Thread(std::bind(&ThreadPool2::threadFunc, this, std::placeholders::_1)));
            int threadId = ptr->getId();
            // C++14 std::unique_ptr<Thread> ptr = make_unique<THread>(std::bind(&ThreadPool::threadFunc, this));
            // C++11 std::make_shared   C++14才更新make_unique

            // 同时unique_ptr禁止了拷贝构造，所以emplace_back应该传入右值
            threads_.emplace(threadId, std::move(ptr));
        }

        /**
         * 为保证线程创建和启动的公平性，统一创建，统一启动
         */

        // 启动所有线程     std::vector<Thread *> threads_;
        for (int i = 0; i < initThreadSize_; i++)
        {
            threads_[i]->start(); // 需要执行一个线程函数
            idleThreadSize_++;
        }
    }

    // 定义任务队列数量上限阈值
    void setTaskQueueMaxThreshHold(int threshHold)
    {
        if (checkRunningState())
            return;
        taskQueueMaxThreshHold_ = threshHold;
    }

    // 定义cached模式下线程阈值
    void setThreadSizeThreshhold(int threshHold)
    {
        if (checkRunningState())
            return;
        if (poolMode_ == PoolMode::MODE_FIXED)
            maxThreadSize_ = threshHold;
    }

    // 给线程池提交任务
    // 使用可变参模板编程，让submitTask可以接收任意任务函数和任意数量的参数
    template <typename Func, typename... Args>
    auto submitTask(Func &&func, Args &&...args) -> std::future<decltype(func(args...))>
    {
        // 打包任务，放入任务队列
        using RType = decltype(func(args...));
        auto task = std::make_shared<std::packaged_task<RType()>>(
            std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
        std::future<RType> result = task->get_future();

        // 获取锁
        std::unique_lock<std::mutex> lock(taskQueueMtx_);
        // 线程的通信    等待任务队列有空余
        //  while(taskCnt_==taskQueueMaxThreshHold_){
        //      notFull_.wait(lock);
        //  }

        // 传入的lambda表达式如果是false，则wait等待
        // notFull_.wait(lock, [&]() -> bool
        //               { return taskCnt_ < taskQueueMaxThreshHold_; });
        // wait  wait_for    wait_until
        // wait:一直等待，直到条件满足，再进行后续操作
        // wait_for:等待有时长限制，比如3s，1s，时间一到，不再等待
        // wait_until:设置等待时间的截止点
        //  用户提交任务最长不能阻塞超过1s，否则判断题脚任务失败
        if (!notFull_.wait_for(lock, std::chrono::seconds(1), [&]() -> bool
                               { return taskQueue_.size() < (size_t)taskQueueMaxThreshHold_; }))
        {
            // 返回false，表示notFull_等待1s，条件依然没有满足
            std::cerr << "task queue is full,submit task fail." << std::endl;
            LOG("task queue is full,submit task fail.");
            auto temp = std::make_shared<std::packaged_task<RType()>>(
                []() -> RType
                { return RType(); });
            return temp->get_future();
        }
        // 如果有空余，把任务放入任务队列中
        //using Task = std::future<void()>;
        //std::queue<Task> taskQueue_; // 任务队列 类型
        taskQueue_.emplace([task](){
            //去执行下面的任务
            (*task)();
        });
        taskCnt_++;
        // 因为新放了任务，任务队列肯定不空了，在notEmpty_上进行通知
        notEmpty_.notify_all();

        // cached模式，任务处理比较紧急 场景：小而快的任务，
        // 需要根据任务数量和空闲线程的数量，判断是否需要创建新的线程出来？
        if (poolMode_ == PoolMode::MODE_CACHED && taskCnt_ > idleThreadSize_ && curThreadSize_ < maxThreadSize_)
        {
            LOG("Create new Thread!!!");
            // 创建新线程
            std::unique_ptr<Thread> ptr(new Thread(std::bind(&ThreadPool2::threadFunc, this, std::placeholders::_1)));
            int threadId = ptr->getId();
            threads_.emplace(threadId, std::move(ptr));
            // 启动新的线程对象
            threads_[threadId]->start();
            // 修改线程个数相关的变量
            idleThreadSize_++;
            curThreadSize_++;
        }

        // 返回任务的 Result 对象
        return result;
    }

    // 禁用拷贝构造函数
    ThreadPool2(const ThreadPool2 &) = delete;

    // 禁用拷贝构造函数
    ThreadPool2 &operator=(const ThreadPool2 &) = delete;

private:
    // 定义线程函数
    void threadFunc(int threadId)
    {
        // 操作过程
#if 0
// 获取锁
std::unique_lock<std::mutex> lock(taskQueueMtx_);
// 判断队列是否为空
// while (taskCnt_ == 0)
// {
//     notEmpty_.wait(lock);
// }
notEmpty_.wait(lock, [&]() -> bool
               { return taskCnt_ > 0; });
// 有生产者放入了任务
std::shared_ptr<Task> task = taskQueue_.front();
task->run();
taskQueue_.pop();
taskCnt_--;
notFull_.notify_all();
#else
        auto lastTime = std::chrono::high_resolution_clock().now();

        // 所有任务必须执行完成，线程池才可以回收所有线程资源
        for (;;)
        {
            ThreadPool2::Task task;
            {
                // 先获取锁
                std::unique_lock<std::mutex> lock(taskQueueMtx_);
                std::cout << "tid:" << std::this_thread::get_id() << "尝试获取任务..." << std::endl;

                // 锁 + 双重判断
                // 以解决 FIXED模式下，在该循环死锁的问题，notify后while条件仍然为true，然后进行wait（）产生死锁
                while (taskCnt_ == 0)
                {
                    // 线程池要结束，回收线程资源
                    if (!isRunning_)
                    {
                        threads_.erase(threadId);
                        exitCond_.notify_all();
                        std::cout << "threadid:" << std::this_thread::get_id() << "exit!!" << std::endl;
                        return; // 线程函数结束，线程结束
                    }
                    if (poolMode_ == PoolMode::MODE_CACHED)
                    {
                        // 条件变量超时返回
                        //  cached模式下，有可能已经创建了很多的线程，但是空闲时间超过60s，
                        //  应该把多余的线程结束回收掉（超过initThreadSize_数量的线程要进行回收）
                        // 当前时间 - 上一次线程执行的时间 > 60s

                        //  每一秒钟返回一次     怎么区分，超时返回？还是有任务待执行返回
                        if (std::cv_status::timeout == notEmpty_.wait_for(lock, std::chrono::seconds(1)))
                        {
                            auto now = std::chrono::high_resolution_clock().now();
                            // 转换为 s
                            auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
                            if (dur.count() >= THREAD_MAX_IDLE_TIME && curThreadSize_ > initThreadSize_)
                            {
                                // 开始回收当前线程
                                // 记录线程数量相关的值的修改
                                // 把线程对象从线程列表容器中删除   没有办法  threadFunc <=> thread 对象
                                // thread_id ---->线程对象
                                threads_.erase(threadId);
                                curThreadSize_--;
                                idleThreadSize_--;
                                std::cout << "threadid:" << std::this_thread::get_id() << "exit!!" << std::endl;
                                return;
                            }
                        }
                    }
                    else
                    {
                        // 等待empty条件
                        notEmpty_.wait(lock);
                    }
                }
                idleThreadSize_--;
                std::cout << "tid:" << std::this_thread::get_id() << "获取任务成功" << std::endl;
                // 从任务队列中取一个任务出来
                task = taskQueue_.front();
                taskQueue_.pop();
                taskCnt_--;

                // 如果依然有剩余任务，继续通知其他的线程执行任务
                if (taskCnt_ > 0)
                    notEmpty_.notify_all();

                // 取出一个任务，进行通知,通知可以继续提交生产任务
                notFull_.notify_all();
            } // 就应该把锁释放掉
            // 当前线程负责执行这个任务
            if (task!=nullptr)
            {
                // task->run();    //执行任务；把任务的返回值通过setVal方法给到Result
                task(); //执行 function<void()>
            }

            idleThreadSize_++;
            // 更新线程执行完的时间
            lastTime = std::chrono::high_resolution_clock().now();
        }
        threads_.erase(threadId);
        exitCond_.notify_all();
        std::cout << "threadid:" << std::this_thread::get_id() << "exit!!" << std::endl;
        return;
    }

    // 检查pool的运行状态
    bool checkRunningState() const
    {
        return isRunning_;
    }

private:
    // TODO:下划线加在命名后面，为了避免与linux系统库产生冲突，开源代码的编码习惯
    // std::vector<std::unique_ptr<Thread>> threads_; // 线程列表
    std::unordered_map<int, std::unique_ptr<Thread>> threads_;
    std::size_t initThreadSize_;     // 初始的线程数量
    std::size_t maxThreadSize_;      // 线程数量上限阈值
    std::atomic_int curThreadSize_;  // 记录当前线程池里面线程的总数量
    std::atomic_int idleThreadSize_; // 记录空闲线程的数量

    // Task任务 =》 函数对象
    using Task = std::function<void()>;
    std::queue<Task> taskQueue_; // 任务队列

    std::atomic_uint taskCnt_;   // 任务的数量
    int taskQueueMaxThreshHold_; // 任务队列数量上限的阈值

    std::mutex taskQueueMtx_; // 保证任务队列的线程安全
    /*
    condition_variable type
    notFull/notEmpty
    */
    std::condition_variable notFull_;  // 表示任务队列不满
    std::condition_variable notEmpty_; // 表示任务队列不空
    std::condition_variable exitCond_; // 等待线程执行完毕

    PoolMode poolMode_; // 线程池的工作模式

    // 表示当前线程池的启动状态
    std::atomic_bool isRunning_;
};

#endif

#endif