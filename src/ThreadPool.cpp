#include "ThreadPool.h"

ThreadPool::ThreadPool(size_t numThreads) {
    for (size_t i = 0; i < numThreads; ++i) {
        workers_.emplace_back([this] {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(mtx_);
                    cv_.wait(lock, [this] {
                        return stop_.load() || !tasks_.empty();
                    });
                    if (stop_ && tasks_.empty()) return;
                    task = std::move(tasks_.front());
                    tasks_.pop();
                    ++activeTasks_;
                }
                task();
                {
                    std::lock_guard<std::mutex> lock(doneMtx_);
                    --activeTasks_;
                }
                doneCv_.notify_all();
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    stop_ = true;
    cv_.notify_all();
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
}

void ThreadPool::waitAll() {
    std::unique_lock<std::mutex> lock(doneMtx_);
    doneCv_.wait(lock, [this] {
        std::lock_guard<std::mutex> taskLock(mtx_);
        return tasks_.empty() && activeTasks_ == 0;
    });
}
