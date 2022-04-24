#include "okage/load_queue.hpp"
#include <array>
#include <atomic>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool value) {if(!value)throw std::runtime_error("Load queue assertion failed");}
struct Signal {
    std::mutex mutex;
    std::condition_variable cv;
    bool set=false;
    void notify() {{std::lock_guard lock(mutex);set=true;}cv.notify_all();}
    void wait() {std::unique_lock lock(mutex);cv.wait(lock,[this]{return set;});}
};
struct OnRelease {
    Signal& signal;
    explicit OnRelease(Signal& s):signal(s) {}
    ~OnRelease(){signal.notify();}
};
template<class F> void rejects(F&& f) {
    bool failed=false;try{f();}catch(const std::runtime_error&){failed=true;}
    require(failed);
}
struct LoadFailure:std::runtime_error {using std::runtime_error::runtime_error;};
}
int main() {
    try {
        {
            okage::NativeLoadQueue queue;
            std::atomic<unsigned> executions=0;
            std::vector<unsigned> order;
            Signal started,release,finished;
            queue.enqueue([&]{started.notify();release.wait();order.push_back(0);++executions;});
            for(unsigned i=1;i<30;++i)queue.enqueue([&,i]{order.push_back(i);++executions;});
            queue.enqueue([&,done=std::make_shared<OnRelease>(finished)]{order.push_back(30);++executions;});
            require(queue.pending()==31&&executions==0); // Paused, no timing assumption.
            rejects([&]{queue.enqueue([]{});});require(queue.pending()==31);
            queue.resume();queue.resume();started.wait();
            require(queue.pending()==31&&executions==0); // One in flight plus30 queued.
            rejects([&]{queue.enqueue([]{});});
            release.notify();finished.wait();queue.check();
            require(queue.pending()==0&&executions==31&&order.size()==31);
            for(unsigned i=0;i<31;++i)require(order[i]==i);
            Signal later;
            queue.enqueue([&,done=std::make_shared<OnRelease>(later)]{order.push_back(31);});
            later.wait();queue.check();require(queue.pending()==0&&order.back()==31);
            bool empty=false;try{queue.enqueue({});}catch(const std::invalid_argument&){empty=true;}
            require(empty&&queue.pending()==0);
        }
        {
            okage::NativeLoadQueue queue;
            Signal failed;
            std::atomic<bool> ran_later=false;
            queue.enqueue([done=std::make_shared<OnRelease>(failed)]{throw LoadFailure("owned asset rejected");});
            queue.enqueue([&]{ran_later=true;});
            queue.resume();failed.wait();
            for(unsigned attempt=0;attempt<2;++attempt) {
                bool caught=false;
                try{queue.check();}catch(const LoadFailure& e){caught=std::string(e.what())=="owned asset rejected";}
                require(caught&&queue.pending()==2&&!ran_later);
            }
            rejects([&]{queue.enqueue([]{});});rejects([&]{queue.resume();});
            require(queue.pending()==2);
        }
        {
            Signal started,release,destroying;
            std::atomic<bool> callback_finished=false,destructor_finished=false;
            auto owner=std::make_shared<unsigned>(0);
            auto queue=std::make_unique<okage::NativeLoadQueue>();
            queue->enqueue([&,owner]{started.notify();release.wait();*owner=73;callback_finished=true;});
            queue->resume();started.wait();
            std::thread destroyer([&]{
                destroying.notify();
                queue.reset();
                require(callback_finished);destructor_finished=true;
            });
            destroying.wait();
            require(!callback_finished&&!destructor_finished);
            release.notify();destroyer.join();
            require(callback_finished&&destructor_finished&&*owner==73);
        }
        {
            // Destruction of a never-resumed queue does not execute its work.
            std::atomic<bool> ran=false;
            {okage::NativeLoadQueue queue;queue.enqueue([&]{ran=true;});}
            require(!ran);
        }
        std::cout<<"Native load queue FIFO, capacity, failure and join tests passed\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
