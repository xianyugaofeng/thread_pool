#ifndef THREAD_POOL_V2_HPP
#define THREAD_POOL_V2_HPP

// 现代化C++17线程池：动态扩缩容+异常安全+优雅关闭+任务依赖(DAG)
//
// 与 thread_pool.hpp(pthread 风格) 的关键区别：
//  1. 异常安全：任务内抛出的异常被存入对应的std::future，由调用方get()时重抛；
//     工作线程的执行层还有catch(...)兜底，任何异常都不会逃逸出线程入口
//     (一旦逃逸，C++运行库会调用std::terminate导致整个进程崩溃)
//  2. 优雅关闭：析构时置 stop 标志并广播唤醒，工作线程只有在队列已排空后才退出
//     因此已提交的任务保证全部执行完，不会丢失；随后析构统一join所有线程
//     也支持 shutdown(false) 放弃模式：丢弃未开始的任务，对应 future 收到 broken_promise
//  3. 动态扩缩容：min_workers常驻；提交时若有积压且无空闲线程则扩容(不超过max_workers)
//     弹性线程空闲超过 keep_alive 后自愿退出，缩回min_workers
//  4. 任务依赖：submit_after({依赖...}, fn)构造DAG。下游任务只有在全部上游完成后才入队，
//     工作线程从不阻塞等待依赖，因此依赖不会消耗/饿死线程
//     上游失败时原始异常会传播给所有下游任务的future，下游的用户代码不再执行

#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

//向已关闭(正在析构/已shutdown)的线程池提交任务时抛出
//公有继承标准库异常 std::runtime_error
//ThreadPoolClosed 是一种 std::runtime_error，也是一种 std::exception；
//可以用catch(const ThreadPoolClosed&)、catch(const std::runtime_error&)、catch(const std::exception&)捕获

class ThreadPoolClosed : public std::runtime_error {
public:
    //默认构造函数，无参数
    //成员初始化列表，调用基类构造函数，把字面量传给std::runtime_error
    //空函数体。所有工作都在基类构造中完成
    ThreadPoolClosed() : std::runtime_error("submit to a closed ThreadPool") {}
};

namespace detail {
// 池内部任务结点，由shared_ptr管理：
// - 池侧：队列queue_/依赖等待表waiting_/保活表retired_持有
// 保活表retired_任务结束且无下游时释放
// - 用户侧：TaskHandle持有(用于 submit_after 表达依赖)
// 字段同步约定：pending_deps / dependents / done / error 只在持有池互斥量时访问；
// error 在任务体运行时被工作线程读写，与池锁之间通过出队时的一次加锁建立 happens-before。
struct Job {
    std::size_t pending_deps = 0;   // 尚未完成的上游依赖数
    std::vector<Job*> dependents;   // 依赖本任务的下游任务(弱引用，指向池持有的Job)
    std::exception_ptr error;       // 非空=已失败：自身异常或上游传播而来
    bool done = false;              // 已运行结束(或因上游失败而未运行即判定失败)
    std::function<void()> call;     // 任务体，执行时绝不向外抛异常
};

} // namespace detail

// 依赖句柄：submit返回的TaskResult::handle()可传给后续submit_after
class TaskHandle {
public:
    //只读、不变的操作：用户能做的是检查是否有效和把它传给submit_after
    TaskHandle() = default;     // 默认构造=无效句柄
    //防止用户误把默认构造或已失效的句柄传给submit_after
    //valid()只是检查shared_ptr非空，不保证对应Job尚未完成
    bool valid() const { return job_ != nullptr; }      // 是否有效
private:
    friend class ThreadPool;
    //用户submit_after时需要原子地拿到Job*去增加pending_deps，必须有稳定对象
    //禁止隐式类型转换 必须显式地传入一个Job的智能指针
    //std::move(job)直接把它内部的指针所有权移动给成员变量 job_
    explicit TaskHandle(std::shared_ptr<detail::Job> job) : job_(std::move(job)) {}
    //智能指针持有job对象，通过引用计数保证job对象在TaskHandle存在期间不会被销毁
    std::shared_ptr<detail::Job> job_;
};

// submit的返回值：future负责结果/异常传递，handle负责依赖表达
template <class R>  //定义返回值类型
class TaskResult {
public:
    //std::future::get()本身是非const之后共享状态被释放 调用后future就失效了，再调用会出错
    //如果异步任务在执行过程中抛出了异常，这个异常会被捕获并存储在 future 中
    //任务体抛出的异常经promise::set_exception存入共享状态，get()在此重抛
    R get() { return fut_.get(); }        // 阻塞取结果；任务抛出的异常在这里重抛
    //只阻塞等待完成，不取值、不消费共享状态、不重抛异常
    //即使异步任务抛出了异常，wait() 也不会抛出异常。它只负责等待任务结束
    void wait() const { fut_.wait(); }    // 只等完成，不取值也不重抛
    //返回shared_ptr<Job>的拷贝（原子递增引用计数）避免用户拿到悬垂引用（TaskResult销毁后引用仍有效）
    //每次返回一个新的TaskHandle，都指向同一Job
    TaskHandle handle() const { return handle_; }

private:
    //与TaskHandle一致，只有池能构造有效TaskResult
    friend class ThreadPool;
    //默认构造被抑制（有用户声明构造函数）
    TaskResult(std::future<R> fut, TaskHandle handle)
        : fut_(std::move(fut)), handle_(std::move(handle)) {}
    std::future<R> fut_;
    TaskHandle handle_;
};

class ThreadPool {
public:
    //动态池：minThreads 常驻，忙时自动扩到 maxThreads，空闲 keepAlive 后缩回
    explicit ThreadPool(std::size_t minThreads, std::size_t maxThreads,
                        std::chrono::milliseconds keepAlive = std::chrono::milliseconds(3000))
        : min_workers_(minThreads), max_workers_(maxThreads), keep_alive_(keepAlive) {
        if (minThreads == 0 || maxThreads < minThreads)
            throw std::invalid_argument("ThreadPool: require 1 <= min <= max");

        // 构造函数抛异常时析构函数不会执行，必须手动回收已创建的线程，
        // 否则joinable的std::thread被析构会直接std::terminate
        bool spawn_failed = false;
        {
            //用RAII方式管理互斥锁
            //进入作用域时加锁，离开作用域时自动解锁，即使函数提前return或抛出异常，也不会忘记解锁
            //用一个局部对象接管mtx_ 的加锁/解锁生命周期，避免手动写mtx_.lock()和mtx_.unlock()
            std::unique_lock<std::mutex> lock(mtx_);
            //此刻持有mtx_，而shutdown需要lock+join，会死锁
            for (std::size_t i = 0; i < min_workers_; ++i) {
                try {
                    spawn_locked();
                } catch (...) {
                    spawn_failed = true;
                    break;
                }
            }
        }
        if (spawn_failed) {
            shutdown(true);   // 锁外收尾：唤醒并join已创建的线程
            throw std::runtime_error("ThreadPool: failed to create worker threads");
        }
    }

    //静态池便捷构造：线程数固定（不会扩缩容）
    //idleTimeout设为0，表示空闲线程不会被回收
    explicit ThreadPool(std::size_t fixedThreads)
        : ThreadPool(fixedThreads, fixedThreads, std::chrono::milliseconds(0)) {}

    ~ThreadPool() { shutdown(true); }   // 优雅关闭：排空队列 → 线程退出 → join

    //拷贝构造函数删除
    ThreadPool(const ThreadPool&) = delete;            // 线程内部持有 this，禁止复制/移动
    //拷贝赋值运算符删除
    ThreadPool& operator=(const ThreadPool&) = delete;

    // 提交任务。返回TaskResult<R>：get()取结果或重抛任务内异常
    template <class F, class... Args>
    //返回类型依赖于模板参数，必须写在参数列表之后
    //万能引用
    auto submit(F&& f, Args&&... args)
        //对类型做退化处理，去掉引用、cv限定，数组/函数退化为指针
        //计算F以Args...为参数调用后的返回类型
        //外层再包一层TaskResult<...>，即最终返回一个能取结果/重抛异常的任务句柄
        -> TaskResult<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>> {
        //submit本身不干活，只是把参数原样转发给真正的实现函数submit_impl
        //std::forward保留参数的左值/右值属性，避免不必要的拷贝/移动
        //第一个参数传了一个空的std::vector<TaskHandle>{}看起来是表示这个任务没有前置依赖
        return submit_impl(std::vector<TaskHandle>{}, std::forward<F>(f),
                           std::forward<Args>(args)...);
    }

    // 提交带依赖的任务：deps全部成功完成后才执行
    // 上游任一失败 → 本任务不执行用户代码，其future收到上游的原始异常(级联传播)
    template <class F, class... Args>
    auto submit_after(std::vector<TaskHandle> deps, F&& f, Args&&... args)
        -> TaskResult<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>> {
        return submit_impl(std::move(deps), std::forward<F>(f), std::forward<Args>(args)...);
    }

    // 请求关闭（幂等）：
    // drain=true  —— 优雅关闭：已提交(含依赖未就绪)的任务全部执行完后线程退出，并join等待
    // drain=false —— 放弃模式：丢弃未开始的任务，其future收到broken_promise
    // 注意：不要与析构或另一次shutdown并发调用
    void shutdown(bool drain = true) {
        {   
            //在构造时自动锁定mtx_，在析构（离开作用域）时自动解锁
            std::lock_guard<std::mutex> lock(mtx_);
            if (stop_) return;
            stop_ = true;
            if (!drain) {
                abandoned_ = true;
                // 置空任务体即销毁packaged_task → 未运行任务的future变为broken_promise
                // Job壳移入retired_保活：
                // 正在运行的任务完成后仍会解引用dependents里的裸指针，不能立刻释放
                for (auto& j : queue_) {
                    j->call = nullptr;
                    retired_.push_back(std::move(j));
                }
                queue_.clear();
                for (auto& kv : waiting_) {
                    kv.second->call = nullptr;
                    retired_.push_back(std::move(kv.second));
                }
                waiting_.clear();
            }
        }
        //唤醒所有阻塞在条件变量cv_上的工作线程
        //如果先join()，工作线程可能还在cv_.wait()中沉睡，主线程就会永远阻塞在join()上，形成死锁
        cv_.notify_all();
        // stop_之后不再可能有新线程创建(提交接口已关闭)，threads_冻结，可无锁join
        // 自愿缩容退出的线程对象也在threads_里，join立即返回
        for (auto& t : threads_)
            if (t.joinable()) t.join();
    }

    // 运行中调整线程数上下限：立即扩容到新的min；缩容是惰性的(空闲超时后退出)
    void resize(std::size_t minThreads, std::size_t maxThreads) {
        if (minThreads == 0 || maxThreads < minThreads)
            throw std::invalid_argument("resize: require 1 <= min <= max");
        std::lock_guard<std::mutex> lock(mtx_);
        if (stop_) throw ThreadPoolClosed();
        min_workers_ = minThreads;
        max_workers_ = maxThreads;
        while (active_workers_ < min_workers_)
            spawn_locked();
    }

    // 观测接口（用于测试/监控）
    std::size_t active_workers() {
        std::lock_guard<std::mutex> lock(mtx_);
        return active_workers_;
    }
    std::size_t busy_workers() {
        std::lock_guard<std::mutex> lock(mtx_);
        return busy_workers_;
    }
    std::size_t pending_tasks() {   // 队列中的 + 依赖未就绪的
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.size() + waiting_.size();
    }

private:
    // 提交与依赖
    template <class F, class... Args>
    auto submit_impl(std::vector<TaskHandle> deps, F&& f, Args&&... args)
        -> TaskResult<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>> {
        //模板参数R就是被调用函数f的返回类型
        //std::invoke_result_t解决：在编译期间算出这个调用返回什么类型，作为future的元素类型和函数返回类型
        using R = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;

        // 第一层：绑定用户代码。用户异常在这里被记录到job->error并原样重抛
        auto bound = [fn = std::forward<F>(f),
                    //std::make_tuple解决：把一堆参数打包成一个可存储的对象
                    //std::apply里要std::move(fn)和std::move(tup)，lambda需要修改捕获的成员
                      tup = std::make_tuple(std::forward<Args>(args)...)]() mutable -> R {
            //std::apply把tuple展开，等价于调用fn(args...)
            //把一个tuple拆成多个实参，调用函数
            return std::apply(std::move(fn), std::move(tup));
        };
        //make_shared会把对象本身和shared_ptr的控制块放在同一块内存里分配
        auto job = std::make_shared<detail::Job>();
        //wrapper不接受任何参数，调用形式是 wrapper()
        //本次lambda需要调用另一个mutable lambda或者需要修改捕获成员，就必须让operator()非const
        //显式指定返回类型为R
        auto wrapper = [job, bound = std::move(bound)]() mutable -> R {
            // error已在入队前被注入 = 上游依赖失败：
            // 不执行用户代码，直接把上游的原始异常抛给本任务的future
            if (job->error) std::rethrow_exception(job->error);
            try {
                return bound();
            } catch (...) {
                job->error = std::current_exception();  // 记录，供下游传播
                throw;                                   // 重抛 → packaged_task 存入 future
            }
        };
        // 第二层：packaged_task捕获异常到future。用shared_ptr持有
        // 把wrapper包起来，调用后结果/异常会自动存入关联的future
        // std::packaged_task<R()>内部持有一个shared state，它和pt->get_future()拿到的std::future<R>共享同一份状态
        auto pt = std::make_shared<std::packaged_task<R()>>(std::move(wrapper));
        // 因为std::function要求可拷贝而packaged_task只可移动
        std::future<R> fut = pt->get_future();
        //job->call 的类型是 std::function<void()>
        //用lambda捕获shared_ptr<packaged_task>，lambda本身可拷贝，间接调用pt()
        //调用 packaged_task::operator() 时，标准库会执行你存入的任务，也就是这里的 wrapper。它的处理规则是：
        //如果 wrapper 正常返回，返回值写入 shared state；
        //如果 wrapper 抛出异常，packaged_task 会捕获这个异常，把它存进 shared state
        job->call = [pt]() { (*pt)(); };   // 执行入口：pt() 保证不向外抛

        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (stop_) throw ThreadPoolClosed();
            //把当前任务注册到所有上游依赖的dependents列表，并设置job->pending_deps（未完成依赖计数）
            //job是std::shared_ptr<detail::Job>，所以需要.get()取出裸指针
            link_dependencies_locked(job.get(), deps);
            if (job->pending_deps == 0) {
                schedule_locked(job);            // 无未完成依赖(或依赖已全部失败) → 直接调度
            } else {
                waiting_.emplace(job.get(), job); // 否则寄存，等最后一个依赖完成时再入队
            }
        }
        return TaskResult<R>(std::move(fut), TaskHandle(job));
    }

    // 持锁调用：把job挂到各上游依赖的dependents上，统计未完成依赖数
    // 依赖已失败的任务直接注入异常(入队后 wrapper 会先检查并转抛)
    // 注意：句柄必须来自本池且先校验后登记
    // 若登记中途抛异常，已写入上游dependents的裸指针会随job一起被释放而悬空
    // (句柄也不支持跨池使用：两个池的生命周期无法互相保证Job存活)
    void link_dependencies_locked(detail::Job* job, const std::vector<TaskHandle>& deps) {
        for (const TaskHandle& d : deps)
            if (!d.valid()) throw std::invalid_argument("submit_after: invalid TaskHandle");

        for (const TaskHandle& d : deps) {
            detail::Job* up = d.job_.get();
            if (up->done) {
                if (up->error && !job->error) job->error = up->error;
            } else {
                up->dependents.push_back(job);
                ++job->pending_deps;
            }
        }
    }

    // 持锁调用：把任务放入执行队列并唤醒/扩容。放弃模式下仅保活不再执行。
    void schedule_locked(std::shared_ptr<detail::Job> job) {
        if (abandoned_) {
            job->call = nullptr;
            retired_.push_back(std::move(job));
            return;
        }
        queue_.push_back(std::move(job));
        cv_.notify_one();
        maybe_spawn_locked();
    }

    // 持锁调用：任务已完成(成功或失败)。唤醒下游：全部依赖就绪的移入执行队列；
    // 本任务失败时把异常注入下游(下游 wrapper 检查到后不执行用户代码直接转抛)
    void finish_locked(const std::shared_ptr<detail::Job>& job) {
        job->done = true;
        for (detail::Job* d : job->dependents) {
            if (job->error && !d->error) d->error = job->error;
            auto it = waiting_.find(d);
            //find未命中时返回end()迭代器
            if (it == waiting_.end()) continue;   // 防御：正常流程不会发生
            if (--d->pending_deps == 0) {
                std::shared_ptr<detail::Job> dp = std::move(it->second);
                waiting_.erase(it);
                schedule_locked(std::move(dp));
            }
        }
        job->dependents.clear();
        job->call = nullptr;   // 释放任务体(结果/异常已在 future 里)
    }

    // 工作线程
    void worker_loop() {
        std::unique_lock<std::mutex> lock(mtx_);
        for (;;) {
            if (queue_.empty()) {
                if (active_workers_ > min_workers_) {
                    // 弹性线程：带超时等待。空闲超过keep_alive且仍无事可做 → 自愿退出(缩容)。
                    // 退出时只减计数不join：线程对象留在threads_，由shutdown/析构统一join
                    // 捕获this是为了访问成员
                    bool ready = cv_.wait_for(lock, keep_alive_,
                        //!queue_.empty()有新任务待处理
                                              [this] { return stop_ || !queue_.empty(); });
                    if (!ready && !stop_ && queue_.empty()
                        && active_workers_ > min_workers_) {
                        --active_workers_;
                        break;
                    }
                } else {
                    // 常驻线程：无限等待直到有任务或停止
                    cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
                }
                if (queue_.empty()) {
                    if (stop_) break;   // 优雅退出的唯一出口：stop 且队列已排空 → 任务不丢失
                    continue;           // 虚假唤醒或缩容条件已变化 → 重新等待
                }
            }
            // ---- 队列非空：取任务执行 ----
            std::shared_ptr<detail::Job> job = std::move(queue_.front());
            queue_.pop_front();
            ++busy_workers_;
            lock.unlock();

            execute_job(*job);   // 异常安全：任何异常都挡在这一层，线程绝不因此崩溃

            lock.lock();
            --busy_workers_;
            finish_locked(job);  // 标记完成并派生下游依赖(可能向队列追加新任务)
        }
    }

    // 锁外执行任务体。正常路径异常已由packaged_task存入future；
    // catch(...)是最后防线(例如任务体本身被置空等意外情况)，保证没有异常逃逸出线程入口
    static void execute_job(detail::Job& job) {
        try {
            if (job.call) job.call();
        } catch (...) {
            // 最后的防线，确保异常不会逃逸出线程入口
        }
    }

    // 扩缩容
    // 持锁调用：未消费的工作量(队列任务+执行中)超过线程数 → 扩容一个线程
    // 不能用busy_workers_判断无空闲：提交爆发期间worker尚未醒来
    // busy计数滞后，会错过扩容窗口；按工作量vs线程数比较则与唤醒时序无关
    void maybe_spawn_locked() {
        if (queue_.size() + busy_workers_ > active_workers_
            && active_workers_ < max_workers_) {
            spawn_locked();
        }
    }

    void spawn_locked() {
        //emplace_back内部若分配或构造失败，thread未进入就绪态即被丢弃或未构造，不会留下joinable的thread
        //emplace_back直接在vector尾部构造一个thread对象，传入lambda表达式作为线程入口函数
        threads_.emplace_back([this] { worker_loop(); });
        ++active_workers_;
    }

    // 状态
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<detail::Job>> queue_;   // 就绪待执行
    std::unordered_map<detail::Job*, std::shared_ptr<detail::Job>> waiting_; // 依赖未就绪
    std::vector<std::shared_ptr<detail::Job>> retired_;  // 放弃模式下仅保活的空壳
    std::vector<std::thread> threads_;   // 创建过的全部线程对象(含已自愿退出的)，统一join
    std::size_t min_workers_;
    std::size_t max_workers_;
    std::size_t active_workers_ = 0;   // 存活线程数(含等待中的弹性线程)
    std::size_t busy_workers_ = 0;     // 正在执行任务的线程数
    std::chrono::milliseconds keep_alive_;
    bool stop_ = false;
    bool abandoned_ = false;
};

#endif // THREAD_POOL_V2_HPP
