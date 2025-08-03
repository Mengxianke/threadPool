#include "ThreadPool.h"
#include <iostream>
#include <chrono>

// 构造函数 - 第一天只是简单的占位实现
ThreadPool::ThreadPool(size_t threads) {
    // construct thread and push it into container
    for (size_t i = 0; i < threads; i++) {
        // std::thread t(workerThread);
        // do not use copy. push_back
        workers.emplace_back(std::thread([this, i] () -> void {
            return this->workerThread(i);
        }));
    }
}   


// resize the number of threads in the worker
void ThreadPool::resize(size_t threads) {
    // main thread
    std::unique_lock lock(queue_mutex);
    auto worker_size = workers.size();
    if (threads > worker_size) {
        // reserve and push new thread into it;
        workers.reserve(threads);
        for (size_t i = worker_size; i < threads; i++) {
            workers.emplace_back(std::thread([this, i]() -> void {
                return this->workerThread(i);
            }));
        };
        std::cout << "增加了 " << (threads - worker_size) << " 个工作线程" << std::endl;
    } else {
        // if threads less than the current worker_size. 
        // find the thread that needs to be stopped
        threadsToStop.clear();
        for (size_t i = threads; i < worker_size; i++) {
            threadsToStop.emplace(i);
        }
        // notify the all the thread. check wait condition
        // first unlock, allow the thread to obatin the lock and do their job
        // noify all thread that threadstoStop have been upated
        lock.unlock();
        condition.notify_all();
        // threads is running, wait stop thread to finish
        for (size_t i = threads; i < worker_size; i++) {
            if (workers[i].joinable()) {
                // block the thread. join the main thread
                workers[i].join();
            }
        }
        // resize the workers. grab the lock fisrt;
        lock.lock();
        workers.resize(threads);
        std::cout << "减少了 " << (worker_size - threads) << " 个工作线程" << std::endl;
    }
}

size_t ThreadPool::getThreadCount() {
    return workers.size();
}

size_t ThreadPool::getTaskCount() {
    std::unique_lock lock(queue_mutex);
    return tasks.size();
}

size_t ThreadPool::getCompletedTaskCount() {
    return metrics.completedTaskCount;
}

size_t ThreadPool::getActiveThreadCount() {
    return metrics.activeThreadCount;
}

size_t ThreadPool::getWaitingThreadCount() {
    return metrics.waitingThreadCount;
}

size_t ThreadPool::getFailedTaskCount() {
    return metrics.failedTaskCount;
}


void ThreadPool::pause() {
    std::unique_lock lock(queue_mutex);
    isPaused = true;
    condition.notify_all();
}

void ThreadPool::resume() {
    std::unique_lock lock(queue_mutex);
    isPaused = false;
    condition.notify_all();
}

// waitForTasks to finish
void ThreadPool::waitForTasks() {
    // block the main thread
    std::unique_lock lock(queue_mutex);
    // use another condition variable to weak the main thread
    // not only the tasks is empty but also no active thread is running.
    wait_condition.wait(lock, [this]() -> bool {
        return (this->tasks.empty() && this->metrics.activeThreadCount == 0) || this->stop;
    });
    std::cout << "所有任务已完成" << std::endl;
}

// clear all tasks in tasks queue
void ThreadPool::clearTasks() {
    std::unique_lock lock(queue_mutex);
    size_t taskCount = tasks.size();

    std::priority_queue<TaskInfo> emptyQueue;
    std::swap(tasks, emptyQueue);
    
    size_t taskCountAfter = tasks.size();

    std::cout << "清空任务队列: " << taskCount << taskCountAfter << " 个任务被移除" << std::endl;
}


// 析构函数 - 第一天只是简单的占位实现
ThreadPool::~ThreadPool() {
    // use one queue_mutex lock
    {
        std::unique_lock lock(queue_mutex);
        stop = true;    
    }
    // weak other thread
    condition.notify_all();
    // wait other thread finish their own tasks, other thread may exeucte some tasks during this momemnt;
    for (std::thread &worker: workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    std::cout << "~ThreadPool";
}  

// enqueue task
// std::future<void> ThreadPool::enqueue(std::function<void()> task) {
//     // create a new task
//     auto _task = std::bind(task);
//     std::packaged_task<void()> packaged_task(_task);
//     // push task to the taskQueue, do not copy one, just use the same task
//     this->tasks.push(std::move(_task));
//     // getFurture
//     return packaged_task.get_future();
// }


// wokrer thread
// used for process task
void ThreadPool::workerThread(size_t id) {
    // loop, and check avaliable task
    while (true) {
        // declare target task
        TaskInfo task {nullptr};
        bool hasTask = false;
        {
            // try to grab the lock, wait
            metrics.waitingThreadCount++;
            // unique lock
            std::unique_lock lock(queue_mutex);
            // when waitfree lock, other thread can enter in this area
            condition.wait(lock, [this, id]() -> bool {
                return this->stop || 
                       !this->tasks.empty() || 
                       !this->isPaused || 
                       threadsToStop.find(id) != threadsToStop.end();
            });
            // if thread_pool stopped, and no tasks in the queue. exit
            if (this->stop && this->tasks.empty()) {
                return;
            }
            // if thread to stop, delete threadId form the vector
            auto iter = threadsToStop.begin(); 
            while (iter != threadsToStop.end()) {
                size_t threadId = *iter;
                if (threadId == id) {
                    iter = threadsToStop.erase(iter);
                } else {
                    iter++;
                }
            }
            // not wait
            metrics.waitingThreadCount--;
            // find task from queue
            if (tasks.size() > 0) {
                task = tasks.top();
                tasks.pop();    
                hasTask = true;        
            }   
        }
        // get startTime
        auto startTime = std::chrono::steady_clock::now();
        // the task is a lamda function, [task] -> { *task(); };
        if (hasTask && task.task != nullptr) {
            // execute task
            // not in sleep
            metrics.activeThreadCount++;
            metrics.updateActiveThreads(metrics.activeThreadCount);
            try {
                task.task();
                metrics.completedTaskCount++;
            } catch(const std::exception& e) {
                std::cerr << "异常发生在任务中: " << e.what() << std::endl;
                metrics.failedTaskCount++;
            } catch(...) {
                std::cerr << "未知异常发生在任务中" << std::endl;
                metrics.failedTaskCount++;
            }
            // update finish task, regardless whether the task failed or not
            // use atomic, get rid of lock
            metrics.activeThreadCount--;
            auto endTime = std::chrono::steady_clock::now();
            // duration Time
            auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - startTime);
            metrics.addTaskTime(duration.count());
            // notify one wait condition
            wait_condition.notify_one();
        }
    }
}   