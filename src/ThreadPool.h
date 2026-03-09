#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <atomic>

/**
 * @brief 轻量级线程池，用于并行瓦片转换任务
 */
class ThreadPool {
public:
    explicit ThreadPool(size_t numThreads);
    ~ThreadPool();

    /**
     * @brief 提交任务，返回 future 用于追踪完成状态
     */
    template<class F, class... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>
    {
        using RetType = std::invoke_result_t<F, Args...>;

        auto task = std::make_shared<std::packaged_task<RetType()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );

        std::future<RetType> result = task->get_future();
        {
            std::unique_lock<std::mutex> lock(mtx_);
            if (stop_) throw std::runtime_error("ThreadPool is stopped");
            tasks_.emplace([task]() { (*task)(); });
        }
        cv_.notify_one();
        return result;
    }

    /**
     * @brief 等待所有任务完成
     */
    void waitAll();

    size_t size() const { return workers_.size(); }

private:
    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex                        mtx_;
    std::condition_variable           cv_;
    std::atomic<bool>                 stop_{false};
    std::atomic<int>                  activeTasks_{0};
    std::condition_variable           doneCv_;
    std::mutex                        doneMtx_;
};
