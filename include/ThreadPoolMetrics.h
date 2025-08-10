#ifndef THREAD_POOL_METRICS_H
#define THREAD_POOL_METRICS_H

#include <atomic>
#include <chrono>
#include <string>

// struct ThreadPoolMetrics {
struct ThreadPoolMetrics {
    std::atomic<size_t> totalTasks{ 0 };           // 总任务数
    std::atomic<size_t> peakThreads{ 0 };          // 峰值线程数
    std::atomic<size_t> peakQueueSize{ 0 };        // 峰值队列大小
    std::atomic<uint64_t> totalTaskTimeNs{ 0 };    // 总任务执行时间（纳秒）
    
    // atomic variable for completedTaskCount
    std::atomic<size_t> completedTaskCount{0};
    // atomic variable for active count;
    std::atomic<size_t> activeThreadCount{0};
    // waiting thread count
    std::atomic<size_t> waitingThreadCount{0};
    // atomici variable for failed tasks count
    std::atomic<size_t> failedTaskCount{0};
    // timeout task
    std::atomic<size_t> timedOutTasks{0};
    std::chrono::steady_clock::time_point startTime;  // 线程池启动时间

    // constructor 
    ThreadPoolMetrics();

    // 更新队列大小并记录峰值
    void updateQueueSize(size_t size);

    // 更新活跃线程数并记录峰值
    void updateActiveThreads(size_t count);

    // 添加任务执行时间
    void addTaskTime(uint64_t timeNs);

    // 获取平均任务执行时间（毫秒）
    double getAverageTaskTime() const;

    // 获取线程池运行时间（秒）
    double getUptime() const;

    // 获取任务吞吐量（每秒任务数）
    double getThroughput() const;

    // 获取性能报告
    std::string getReport() const;




};

#endif // THREAD_POOL_METRICS_H



