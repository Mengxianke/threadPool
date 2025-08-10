#include "ThreadPool.h"
#include <iostream>
#include <chrono>
#include <atomic>

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
        // notify all threads. check wait condition
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
    std::priority_queue<std::shared_ptr<TaskInfo>, std::vector<std::shared_ptr<TaskInfo>>, cmp> emptyQueue;
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


// getTaskStatus by id
TaskStatus ThreadPool::getTaskStatus(const std::string& taskId) {
    auto iter = taskIdMap.find(taskId);
    if (iter == taskIdMap.end()) {
        return TaskStatus::NOT_FOUND;
    }
    auto taskInfoPtr = iter->second; 
    return taskInfoPtr->status;
}

std::string ThreadPool::getTaskStatusString(const std::string& taskId) {
    return taskStatusToString(getTaskStatus(taskId));
}

// cancel a task
bool ThreadPool::cancelTask(const std::string& taskId) {
    // grab the lock to access to race-condition data taskIdMap
    std::unique_lock lock(queue_mutex);
    auto iter = taskIdMap.find(taskId);
    if (iter == taskIdMap.end()) {
        // not in the taskIdMap
        return false;
    }
    std::shared_ptr<TaskInfo> taskInfoPtr = iter->second;
    if (!taskInfoPtr) {
        // no taskInfo
        return false;
    }
    auto taskStatus = taskInfoPtr->status;
    // check if condition meets
    if (taskStatus == TaskStatus::RUNNING) {
        return false;
    }
    // already completed or failed or cancelled
    if (taskStatus == TaskStatus::COMPLETED || 
        taskStatus == TaskStatus::FAILED ||
        taskStatus == TaskStatus::CANCELED
    ) {
        return false;
    }
    taskInfoPtr->status = TaskStatus::CANCELED;
    return true;

}

// executeTimoutTask
void ThreadPool::executeTimoutTask(std::shared_ptr<TaskInfo> taskInfoPtr, bool& isTimeout) {
    // variable for task complted. used to check if task is timout
    // use atomic for race-condition
    std::atomic<bool> taskCompleted = false; 
    // catch error in another thread
    std::exception_ptr taskException = nullptr;
    // if pointer points to null, throw error in current thread.
    if (taskInfoPtr == nullptr) {
        throw std::runtime_error("cannot execute task since no task exist");
    }
    // execute the task in another thread
    // taskComplted will be accessed in another thread.
    auto taskThread = std::thread([taskInfoPtr, &taskCompleted, &taskException]() -> void {
        try {
            auto task = taskInfoPtr->task;
            task();
            taskCompleted = true;
        } catch(...) {
            taskCompleted = true;
            taskException = std::current_exception();
        }
    });
    // detach the current thread. run in background
    taskThread.detach();
    // use current thread to check if task is timeout
    auto startTime = std::chrono::steady_clock::now();
    auto waitUntil = startTime + taskInfoPtr->timeout;
    // taskComplted will be accessed in current thread
    while (std::chrono::steady_clock::now() < waitUntil && !taskCompleted) {
        // sleep 10 milliseconds.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // run out of the loop
    // check if the task is completed or timeout
    if (!taskCompleted) {
        isTimeout = true;
        throw std::runtime_error("task is timeout");
    };
    // if taskCompleted, but has error, rethrow the error
    if (taskException) {
        std::rethrow_exception(taskException);
    }
}


// execute a task
void ThreadPool::executeTask(std::shared_ptr<TaskInfo> taskInfoPtr) {
    // the task is a lamda function, [task] -> { *task(); };
    if (taskInfoPtr && taskInfoPtr->task) { 
        bool isTimeout = false;
        // enter running status
        taskInfoPtr->status = TaskStatus::RUNNING;
        try {
            auto task = taskInfoPtr->task;
            task();
            taskInfoPtr->status = TaskStatus::COMPLETED;
            metrics.completedTaskCount++;
        } catch(const std::exception& e) {
            // process the error
            taskInfoPtr->status = TaskStatus::FAILED;
            taskInfoPtr->errorMessage = e.what();
            // failedTask
            metrics.failedTaskCount++;
            std::cerr << "异常发生在任务中: " + taskInfoPtr->taskId << e.what() << std::endl;
        } catch(...) {
            taskInfoPtr->status = TaskStatus::FAILED;
            taskInfoPtr->errorMessage = "unkown error";
            std::cerr << "未知异常发生在任务中" + taskInfoPtr->taskId << std::endl;
            metrics.failedTaskCount++;
        }
    }
}

// get a task
std::shared_ptr<TaskInfo> ThreadPool::getTask(bool& hasTask) {
    hasTask = false;
    std::shared_ptr<TaskInfo> taskInfoPtr { nullptr };
    while (tasks.size() > 0) {
        taskInfoPtr = tasks.top();
        tasks.pop();
         // check if task is cancelled, if cancelled, look for another task
        if (taskInfoPtr && taskInfoPtr->status == TaskStatus::CANCELED) {
            hasTask = false;
            taskInfoPtr = nullptr;
            continue;
        }
        hasTask = true;
        break;
    }
    return taskInfoPtr;
}

// wokrer thread
// used for process task
void ThreadPool::workerThread(size_t id) {
    // loop, and check avaliable task
    while (true) {
        // declare target task
        std::shared_ptr<TaskInfo> taskInfoPtr { nullptr };
        // declare bool variable hasTask
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
                // terminate the loop
                return;
            }
            // if thread to stop, delete threadId form the vector
            // erase parameter can be a iterator or a element
            threadsToStop.erase(id);
            // not wait
            metrics.waitingThreadCount--;
            taskInfoPtr = getTask(hasTask);
        }
        
        if (hasTask && taskInfoPtr && taskInfoPtr->task) {
            // execute task
            // not in sleep
            metrics.activeThreadCount++;
            metrics.updateActiveThreads(metrics.activeThreadCount);
            // get startTime
            auto startTime = std::chrono::steady_clock::now();
            executeTask(taskInfoPtr);
            auto endTime = std::chrono::steady_clock::now();
            // duration Time
            auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - startTime);
            metrics.addTaskTime(duration.count());
            metrics.activeThreadCount--;
            
            // remove taskId from taskIdMap, after execute the task
            {
                // grab the lock
                std::unique_lock lock(queue_mutex);
                auto taskId = taskInfoPtr->taskId;
                taskIdMap.erase(taskId);
            }
        }
        // wake wait for all tasks
        wait_condition.notify_one();
    }
}   