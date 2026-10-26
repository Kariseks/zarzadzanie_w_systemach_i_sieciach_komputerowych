// ============================================================================
//  thread_pool.h  –  prosta pula watkow o dowolnym rozmiarze
//
//  Watki tworzone sa RAZ w konstruktorze i czekaja na zmiennej warunkowej.
//  runOnAll(job) budzi wszystkie watki, kazdy wywoluje job(idWatku),
//  a metoda wraca dopiero gdy wszystkie skoncza (bariera).
//  Dzieki temu kolejne etapy (splot, normalizacja) nie placa za ponowne
//  tworzenie watkow.
// ============================================================================
#pragma once

#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class ThreadPool {
public:
    explicit ThreadPool(int threads)
    {
        if (threads < 1) threads = 1;
        workers_.reserve(threads);
        for (int i = 0; i < threads; ++i)
            workers_.emplace_back([this, i] { workerLoop(i); });
    }

    ~ThreadPool()
    {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
        }
        cvStart_.notify_all();
        for (auto& t : workers_) t.join();
    }

    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    int size() const { return static_cast<int>(workers_.size()); }

    // Uruchamia job(threadId) na kazdym watku puli i czeka na wszystkie.
    void runOnAll(const std::function<void(int)>& job)
    {
        std::unique_lock<std::mutex> lk(m_);
        job_     = &job;
        pending_ = size();
        error_   = nullptr;
        ++generation_;                       // "nowe zadanie" dla watkow
        cvStart_.notify_all();
        cvDone_.wait(lk, [this] { return pending_ == 0; });
        job_ = nullptr;
        if (error_) std::rethrow_exception(error_);
    }

private:
    void workerLoop(int id)
    {
        std::uint64_t seen = 0;
        for (;;) {
            const std::function<void(int)>* job = nullptr;
            {
                std::unique_lock<std::mutex> lk(m_);
                cvStart_.wait(lk, [&] { return stop_ || generation_ != seen; });
                if (stop_) return;
                seen = generation_;
                job  = job_;
            }

            std::exception_ptr err;
            try {
                (*job)(id);
            } catch (...) {
                err = std::current_exception();
            }

            {
                std::lock_guard<std::mutex> lk(m_);
                if (err && !error_) error_ = err;
                if (--pending_ == 0) cvDone_.notify_one();
            }
        }
    }

    std::vector<std::thread>         workers_;
    std::mutex                       m_;
    std::condition_variable          cvStart_;
    std::condition_variable          cvDone_;
    const std::function<void(int)>*  job_ = nullptr;
    std::uint64_t                    generation_ = 0;
    int                              pending_ = 0;
    bool                             stop_ = false;
    std::exception_ptr               error_;
};
