#include "threadpool.h"
#include <iostream>

const int TASK_MAX_THRESHHOLD = 4;
const int THREAD_MAX_THRESHHOLD = 10;
const int THREAD_MAX_IDLE_TIME = 10; // 单位：秒

// 线程池构造
ThreadPool::ThreadPool()
    : initThreadSize_(0), taskCnt_(0), taskQueueMaxThreshHold_(TASK_MAX_THRESHHOLD), poolMode_(PoolMode::MODE_FIXED), isRunning_(false), idleThreadSize_(0), maxThreadSize_(THREAD_MAX_THRESHHOLD), curThreadSize_(0)
{
}

// 线程池析构
ThreadPool::~ThreadPool()
{
    isRunning_ = false;
    // 等待线程池里面所有的线程返回  有两种状态：阻塞 & 正在执行任务中
    std::unique_lock<std::mutex> lock(taskQueueMtx_);
    notEmpty_.notify_all();
    exitCond_.wait(lock, [&]() -> bool
                   { return threads_.size() == 0; });
}

bool ThreadPool::checkRunningState() const
{
    return isRunning_;
}

// 设置线程池的工作模式
void ThreadPool::setMode(PoolMode mode)
{
    if (checkRunningState())
        return;
    poolMode_ = mode;
}

// 开始线程池
void ThreadPool::start(int initThreadSize)
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
        std::unique_ptr<Thread> ptr(new Thread(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1)));
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

// 定义线程函数     线程池的所有线程从任务队列里面消费任务
void ThreadPool::threadFunc(int threadId) // 线程函数结束了，对应的线程也就结束了
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
        std::shared_ptr<Task> task;
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
                    return; //线程函数结束，线程结束
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
        if (task != nullptr)
        {
            // task->run();    //执行任务；把任务的返回值通过setVal方法给到Result
            task->exec();
        }

        idleThreadSize_++;
        // 更新线程执行完的时间
        lastTime = std::chrono::high_resolution_clock().now();
    }
    threads_.erase(threadId);
    exitCond_.notify_all();
    std::cout << "threadid:" << std::this_thread::get_id() << "exit!!" << std::endl;
    return;
#endif
}

// 定义任务队列数量上限阈值
void ThreadPool::setTaskQueueMaxThreshHold(int threshHold)
{
    if (checkRunningState())
        return;
    taskQueueMaxThreshHold_ = threshHold;
}

// 定义任务队列数量上限阈值
void ThreadPool::setThreadSizeThreshhold(int threshHold)
{
    if (checkRunningState())
        return;
    if (poolMode_ == PoolMode::MODE_FIXED)
        maxThreadSize_ = threshHold;
}

// 给线程池提交任务     用户调用该接口，传入任务对象，生产任务
// 返回值的问题！！！！！
// 如果返回值类型直接定义为 Result，那么将会报错显示，拷贝构造函数被删除，为什么不直接调用移动构造函数？？？？
// C++高版本已解决，11为什么不行？
std::shared_ptr<Result> ThreadPool::submitTask(std::shared_ptr<Task> sp)
{
#if 0
    //获取锁
    std::unique_lock<std::mutex> lock(taskQueueMtx_);
    //线程的通信    等待任务队列有空余
    while(taskCnt_>=taskQueueMaxThreshHold_){
        notFull_.wait(lock);
    }
    //如果有空余，把任务放入任务队列中
    taskQueue_.push(sp);
    //因为新放了任务，任务队列肯定不空了，在notEmpty_上进行通知
    notEmpty_.notify_all();
#else
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
        // 返回 Task 还是 Result
        /**
         * 两种方式
         * return task->getResult();    不可以，线程池执行完该任务task，task对象就被析构掉了
         * return Result(task);
         */

        return std::make_shared<Result>(sp, false);
    }
    // 如果有空余，把任务放入任务队列中
    taskQueue_.emplace(sp);
    taskCnt_++;
    // 因为新放了任务，任务队列肯定不空了，在notEmpty_上进行通知
    notEmpty_.notify_all();

    // cached模式，任务处理比较紧急 场景：小而快的任务，
    // 需要根据任务数量和空闲线程的数量，判断是否需要创建新的线程出来？
    if (poolMode_ == PoolMode::MODE_CACHED && taskCnt_ > idleThreadSize_ && curThreadSize_ < maxThreadSize_)
    {
        LOG("Create new Thread!!!");
        // 创建新线程
        std::unique_ptr<Thread> ptr(new Thread(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1)));
        int threadId = ptr->getId();
        threads_.emplace(threadId, std::move(ptr));
        // 启动新的线程对象
        threads_[threadId]->start();
        // 修改线程个数相关的变量
        idleThreadSize_++;
        curThreadSize_++;
    }

    // 返回任务的 Result 对象
    return std::make_shared<Result>(sp);
#endif
}

/////////////// 线程方法实现
int Thread::generateId_ = 0;

// 线程构造
Thread::Thread(ThreadFunc func)
    : func_(func), threadId_(generateId_++)
{
}

// 线程析构
Thread::~Thread()
{
}

// 启动线程
void Thread::start()
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

int Thread::getId() const
{
    return threadId_;
}

/////////////////   Task方法实现
Task::Task()
    : result_(nullptr)
{
}

void Task::exec()
{
    if (result_ != nullptr)
        result_->setVal(run()); // 这里发生多态调用
}

void Task::setResult(Result *res)
{
    result_ = res;
}

/////////////////   Result方法的实现
Result::Result(std::shared_ptr<Task> task, bool isValid)
    : isValid_(isValid), task_(task), any_(nullptr)
{
    task->setResult(this);
}

Result::~Result()
{
    LOG("result destroyed!!!");
}

Any Result::get()
{
    if (!isValid_)
    {
        return nullptr;
    }
    sem_.wait(); // task任务如果没有执行完，这里会阻塞用户的线程
    return std::move(any_);
}

void Result::setVal(Any any)
{
    // 存储task的返回值
    this->any_ = std::move(any);
    // 已经获取任务的返回值，增加信号量的资源
    this->sem_.post();
}
