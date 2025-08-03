#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <vector>
#include <queue>
#include <unordered_set>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <functional>
#include <stdexcept>
#include <atomic>
#include "TaskInfo.h"
#include <iostream>
#include "ThreadPoolMetrics.h"

class ThreadPool {
public:
    // 构造函数，创建指定数量的工作线程
    ThreadPool(size_t threads);
    
    // 禁用拷贝构造函数和赋值操作符
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    
    // 析构函数
    ~ThreadPool();
    // pause workers
    void pause();
    // remsue workers
    void resume();
    // enqueue task
    // std::future<void> enqueue(std::function<void()> task);
    // template<class F, class... Args>
    // auto enqueue(F&& f, Args&&... args) -> std::future<typename std::invoke_result<F, Args...>::type>;
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<typename std::invoke_result<F, Args...>::type>;

    template<class F, class... Args>
    auto enqueueWithPriority(TaskPriority priorty, F&& f, Args&&... args) -> std::future<typename std::invoke_result<F, Args...>::type>;

    template<class F, class... Args>
    auto enqueueWithInfo(std::string taskId, std::string description, TaskPriority priority, F&&f, Args&&... args) -> std::future<typename std::invoke_result<F, Args...>::type>;


    // public method
    bool isStopped() const { return stop; }
    // get total number of thread in workers
    size_t getThreadCount();
    // get number of task in the taskQueue
    size_t getTaskCount();
    // get the number of completed task count
    size_t getCompletedTaskCount();
    // get active thread count, some tasks sleep or not work, only count thread that execute task
    size_t getActiveThreadCount();
    // waiting thread count
    size_t getWaitingThreadCount();
    // get failed task count
    size_t getFailedTaskCount();
    // wait for all tasks in taskQueue to finish
    void waitForTasks();
    // clear all tasks
    void clearTasks();
    // resize thread count in worker
    void resize(size_t threads);
    //
    std::string getMetricsReport() {
        return this->metrics.getReport();
    }

private:
    // 工作线程容器
    std::vector<std::thread> workers;

    // threads to stop when resize the worker
    std::unordered_set<size_t> threadsToStop;

    // 任务队列
    // std::queue<function<void()>> tasks;
    std::priority_queue<TaskInfo> tasks;
        
    // 同步机制
    std::mutex queue_mutex; 
    // use one condition_variable to wake or block thread for different conditions
    std::condition_variable condition;
    // wait for all tasks to finish variable condition
    std::condition_variable wait_condition;
    
    // 控制线程池停止
    std::atomic<bool> stop{false};
    // worker Thread
    void workerThread(size_t id);
    // status
    bool isPaused = false;
    // metrics
    ThreadPoolMetrics metrics;
};


template<class F, class... Args>
    auto ThreadPool::enqueueWithInfo(
        std::string taskId, 
        std::string description, 
        TaskPriority priority, 
        F&& f, 
        Args&&... args) -> std::future<typename std::invoke_result<F, Args...>::type> {
        using return_type = typename std::invoke_result<F, Args...>::type;
        // taskPromise, use shared_ptr to control the lifecylce of taskPromise
        // different place will use taskPromise;
        std::shared_ptr<std::promise<return_type>> taskPromise = std::make_shared<std::promise<return_type>>();
        // get future
        auto future = taskPromise->get_future();
        // create a task
        std::function<return_type()> _task = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
        // task, use shared_ptr, lamada function use copy. more pointer points to taskPromise.
        // all shared_ptr destory, free promise object.
        std::function<void()> task = [
            _task, 
            taskPromise]() -> void  {
                try {
                    if constexpr (std::is_void_v<return_type>) {
                        _task();
                        taskPromise->set_value();
                    } else {
                        return_type result = _task();
                        taskPromise->set_value(result);
                    }
                } catch  (const std::exception& e) {
                    // promise set_exception
                    std::cout << e.what()  << std::endl;
                    taskPromise->set_exception(std::current_exception());
                    // throw the error
                    throw;
                } catch(...) {
                    std::cout << "throw exception 2" << std::endl;
                    taskPromise->set_exception(std::current_exception());
                    throw;
                }
            };
        {
            // grab the lock
            std::unique_lock lock(queue_mutex);
            // is stop
            if(stop) {
                throw std::runtime_error("enqueue on stopped ThreadPool");
            };

            TaskInfo newTask = {
                std::move(task),
                priority,
                taskId,
                description,
            };

            // emplace the taskInfo
            tasks.emplace(std::move(newTask));
            metrics.totalTasks++;
            metrics.updateQueueSize(tasks.size());
        }
        // weak one thread to process the task
        condition.notify_one();
        return future;
}


template<class F, class... Args>
    auto ThreadPool::enqueueWithPriority(TaskPriority priorty, F&& f, Args&&... args) -> std::future<typename std::invoke_result<F, Args...>::type> {
        return enqueueWithInfo("", "", priorty, std::forward<F>(f), std::forward<Args>(args)...);
} 


template<class F, class... Args>
    auto ThreadPool::enqueue(F&& f, Args&&... args) -> std::future<typename std::invoke_result<F, Args...>::type> {
    // get a function return type
    std::cout << "Enqueue func called";
    return enqueueWithInfo("", "", TaskPriority::MEDIUM, std::forward<F>(f), std::forward<Args>(args)...);
}


// use function template to customize different function with different arguments and return type
// use forward to matin lvalue or rvalue of paramter
// template<class F, class... Args>
//     auto ThreadPool::enqueue(F&& f, Args&&... args) -> std::future<typename std::invoke_result<F, Args...>::type> {
//     // get a function return type
//     using return_type = typename std::invoke_result<F, Args...>::type;
//     // create a function object, use later, call fn() directly. bind function and paramters
//     // use forward to matiain lvalue or rvalue;
//     std::function<return_type()> task = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
//     // us packaged_task to wrap tasks to get a future
//     // std::packaged_task<return_type()> packaged_task(task);

//     // use shared_ptr to control the lifecycle of packaged_task
//     // is a function pointer, return_type is a type of return, () is a paramter
//     auto _task = std::make_shared<std::packaged_task<return_type()>>(std::move(task));

//     std::future<return_type> result = _task->get_future();
//     // use queue_mutex lock
//     {
//         std::unique_lock lock(queue_mutex);
//         // if stop throw error
//         if(stop) {
//             throw std::runtime_error("enqueue on stopped ThreadPool");
//         }
//         // Semplace a lamda function to wrap _task.
//         // when pop the _task out, can call fn() to execute a task.
//         tasks.emplace([_task] { (*_task)(); });
//     }
//     // weak any one thread to get task from taskQueue
//     condition.notify_one();
//     return result;
// }






#endif // THREAD_POOL_H
