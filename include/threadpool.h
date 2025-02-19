#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <vector>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <thread>
#include <public.h>
#include <unordered_map>

/*
模版代码的实现只能写在头文件中
编译阶段进行实例化，实例化之后才能产生真正的可执行的函数
*/
// Any类型：可以接受任意数据的类型
class Any
{
public:
    Any() = default;

    ~Any() = default;

    // 禁止左值构造函数，因为该类中有一个unique_ptr，该成员是禁止左值构造的
    //拷贝构造
    Any(const Any &) = delete;
    Any &operator=(const Any &) = delete;
    // 右值构造
    //移动构造
    Any(Any &&) = default;
    Any &operator=(Any &&) = default;

    // 这个构造函数可以让Any类型接受任意其他的数据
    template <typename T>
    Any(T data) : base_(new Derive<T>(data)) // C++14 std::make_unique<Derive<T>>(data)
    {
    }

    // 这个方法能把Any对象里面存储的data数据提取出来
    template <typename T>
    T cast_()
    {
        // 我们怎么从base_找到他所指向的Derive对象，从它里面取出data成员变量
        // 基类指针 =》 派生类指针      RTTI类型识别转换
        Derive<T> *pd = dynamic_cast<Derive<T> *>(base_.get());
        if (pd == nullptr)
        {
            // 转换失败
            throw "type is incompatible!";
        }
        return pd->data_;
    }

private:
    // 基类类型
    class Base
    {
    public:
        // 如果基类对应的派生类对象是在堆上创建的，delete基类指针，那么派生类的析构函数不会调用
        // 所以这里需要将基类的析构函数实现为虚函数-----》派生类创建在堆上时，删除基类指针
        virtual ~Base() = default;
    };

    // 派生类类型
    template <typename T>
    class Derive : public Base
    {
    public:
        Derive(T data) : data_(data)
        {
        }
        T data_; // 保存了所谓的任意的其他类型
    };

private:
    // 定义一个基类的指针
    std::unique_ptr<Base> base_;
};

// 实现一个信号量类
class Semaphore
{
public:
    Semaphore() : resLimit_(0)
    {
    }
    ~Semaphore() = default;

    // 获取一个信号量资源
    void wait()
    {
        std::unique_lock<std::mutex> lock(mtx_);
        // 等待信号量有资源，没有资源的话，会阻塞当前线程
        cond_.wait(lock, [&]() -> bool
                   { return resLimit_ > 0; });
        resLimit_--;
    }

    // 增加一个信号量资源
    void post()
    {
        std::unique_lock<std::mutex> lock(mtx_);
        resLimit_++;
        //linux下条件变量析构什么也没做，
        //导致这里状态已经失效，无效阻塞
        cond_.notify_all(); //通知，notify_all阻塞
    }

private:
    int resLimit_;
    std::mutex mtx_;
    std::condition_variable cond_;
};

// Task类型的前置声明
class Task;

// 实现接收提交到线程池的task任务执行完成后的返回值类型Result
class Result
{
public:
    Result(std::shared_ptr<Task> task, bool isValid = true);

    Result(const Result&) = delete;
    Result& operator=(const Result&) = delete;
    //显式声明一下移动构造函数
    Result(Result&& res) = default;
    Result& operator=(Result&&) = delete;
    ~Result();

    //问题一：setVal方法，获取任务执行完的返回值
    void setVal(Any any);

    //问题二：get方法，用户调用这个方法获取task的返回值
    Any get();

private:
    Any any_;                    // 存储返回值
    Semaphore sem_;              // 线程通信信号量，保证任务执行完毕后再拿取结果
    std::shared_ptr<Task> task_; // 指向对应获取返回值的任务对象
    std::atomic_bool isValid_;   // 返回值是否有效，如果提交任务失败，那么调用Result.get()不用阻塞
};

// 任务抽象基类
// 用户可以自定义任意任务类型
class Task
{
public:
    Task();

    ~Task() = default;
    // 用户可以自定义任意任务类型，从task继承，重写run方法，实习自定义任务处理
    /**
    返回值类型不使用模版的原因
    》〉》〉编译器对代码段进行编译，从上往下进行，当对纯虚函数进行编译时，会构造虚函数表，
    此时编译器去寻找虚函数的重载实现时，会发现寻找对应的函数失败，出现编译错误
     */
    virtual Any run() = 0;

    void exec();

    void setResult(Result* res);

private:
    Result* result_;    //Result对象的生命周期 》 Task的 ，这里不能使用智能指针（智能指针的循环引用问题）
};

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
    Thread(ThreadFunc func);

    // 线程析构
    ~Thread();

    // 启动线程
    void start();

    //获取线程id
    int getId() const;

private:
    ThreadFunc func_;
    static int generateId_;
    int threadId_;  //保存线程id，方便后续删除线程对象
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
class ThreadPool
{
public:
    // 线程池构造
    ThreadPool();

    // 线程池析构
    ~ThreadPool();

    // 设置线程池的工作模式
    void setMode(PoolMode mode);

    // 开始线程池,默认大小为CPU核心数量
    void start(int initThreadSize = std::thread::hardware_concurrency());

    // 定义任务队列数量上限阈值
    void setTaskQueueMaxThreshHold(int threshHold);

    // 定义cached模式下线程阈值
    void setThreadSizeThreshhold(int threshHold);

    // 给线程池提交任务
    std::shared_ptr<Result> submitTask(std::shared_ptr<Task> sp);

    // 禁用拷贝构造函数
    ThreadPool(const ThreadPool &) = delete;

    // 禁用拷贝构造函数
    ThreadPool &operator=(const ThreadPool &) = delete;

private:
    // 定义线程函数
    void threadFunc(int threadId);

    // 检查pool的运行状态
    bool checkRunningState() const;

private:
    // TODO:下划线加在命名后面，为了避免与linux系统库产生冲突，开源代码的编码习惯
    // std::vector<std::unique_ptr<Thread>> threads_; // 线程列表
    std::unordered_map<int,std::unique_ptr<Thread>> threads_;
    std::size_t initThreadSize_;                   // 初始的线程数量
    std::size_t maxThreadSize_;                     //线程数量上限阈值
    std::atomic_int curThreadSize_;             //记录当前线程池里面线程的总数量
    std::atomic_int idleThreadSize_;            //记录空闲线程的数量

    /*
    如果用户传入的任务对象为临时对象，也就是run函数还未执行完毕task指针已经析构
    我们需要考虑的是延长任务的生命周期直到run函数完全执行完毕
    使用裸指针是不可以的，所以这里使用智能指针
    */
    std::queue<std::shared_ptr<Task>> taskQueue_; // 任务队列
    std::atomic_uint taskCnt_;                    // 任务的数量
    int taskQueueMaxThreshHold_;                  // 任务队列数量上限的阈值

    std::mutex taskQueueMtx_; // 保证任务队列的线程安全
    /*
    condition_variable type
    notFull/notEmpty
    */
    std::condition_variable notFull_;  // 表示任务队列不满
    std::condition_variable notEmpty_; // 表示任务队列不空
    std::condition_variable exitCond_; // 等待线程执行完毕

    PoolMode poolMode_;                // 线程池的工作模式

    // 表示当前线程池的启动状态
    std::atomic_bool isRunning_;
};

#endif