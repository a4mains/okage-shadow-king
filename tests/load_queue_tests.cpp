#include "okage/load_queue.hpp"
#include <array>
#include <atomic>
#include <barrier>
#include <iostream>
#include <memory>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool value,std::source_location location=std::source_location::current()) {
    if(!value)throw std::runtime_error("Load queue assertion failed at line "+
                                       std::to_string(location.line()));
}
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

void wait_for_event(const okage::NativeLoadQueue& queue,okage::LoadEventKind kind) {
    for(;;) {
        const auto snapshot=queue.observation();
        for(const auto& event:snapshot.events)if(event.kind==kind)return;
        std::this_thread::yield();
    }
}

std::size_t event_index(const okage::LoadObservation& snapshot,okage::LoadEventKind kind,
                        std::uint64_t job=0) {
    for(std::size_t i=0;i<snapshot.events.size();++i)
        if(snapshot.events[i].kind==kind&&(!job||snapshot.events[i].job==job))return i;
    throw std::runtime_error("Expected loader event was not recorded");
}

void cooperative_pause_and_checkpoint() {
    using namespace okage;
    NativeLoadQueue queue(128);
    Signal entered,finished;
    std::atomic<bool> second_ran=false;
    const LoadJobIdentity first_identity{14,118},second_identity{15,222};
    const auto first=queue.enqueue([&]{
        entered.notify();
        wait_for_event(queue,LoadEventKind::PauseRequest);
        require(queue.checkpoint());
        require(queue.checkpoint()); // A checkpoint without a pause is a no-op.
    },first_identity);
    const auto second=queue.enqueue([&]{second_ran=true;},second_identity);
    queue.resume();entered.wait();
    queue.pause_and_wait();
    auto paused=queue.observation();
    const auto pause=event_index(paused,LoadEventKind::PauseRequest);
    const auto ack=event_index(paused,LoadEventKind::PauseAcknowledged);
    require(paused.events[pause].job==first&&paused.events[pause].identity==first_identity);
    require(paused.events[ack].job==first&&paused.events[ack].identity==first_identity);
    require(paused.events[ack].pending==2&&paused.events[ack].running_job==first);
    require(paused.events[event_index(paused,LoadEventKind::Checkpoint,first)].thread!=
            std::this_thread::get_id());
    require(queue.pending()==2&&!second_ran);
    // A new enqueue while paused remains behind the already-pending second job.
    const auto third=queue.enqueue([done=std::make_shared<OnRelease>(finished)]{},LoadJobIdentity{16,333});
    require(third==second+1&&queue.pending()==3&&!second_ran);
    queue.resume();finished.wait();queue.check();
    require(queue.pending()==0&&second_ran);
    const auto complete=queue.observation();
    require(event_index(complete,LoadEventKind::WorkComplete,first)<
            event_index(complete,LoadEventKind::WorkStart,second));
    require(event_index(complete,LoadEventKind::WorkStart,second)<
            event_index(complete,LoadEventKind::WorkStart,third));
}

void pause_completion_and_failure_races() {
    using namespace okage;
    {
        NativeLoadQueue queue(64);
        Signal entered,release,finished;
        std::atomic<bool> later_ran=false;
        const auto first=queue.enqueue([&]{entered.notify();release.wait();},{});
        const auto second=queue.enqueue([&]{later_ran=true;},{});
        queue.resume();entered.wait();
        std::jthread pauser([&]{queue.pause_and_wait();});
        wait_for_event(queue,LoadEventKind::PauseRequest);
        release.notify();
        pauser.join();
        const auto paused=queue.observation();
        require(event_index(paused,LoadEventKind::WorkComplete,first)<
                event_index(paused,LoadEventKind::PauseAcknowledged));
        require(paused.events[event_index(paused,LoadEventKind::PauseAcknowledged)].job==0);
        require(queue.pending()==1&&!later_ran);
        queue.enqueue([done=std::make_shared<OnRelease>(finished)]{});
        queue.resume();finished.wait();queue.check();
        require(later_ran&&queue.pending()==0);
        require(event_index(queue.observation(),LoadEventKind::WorkStart,second)<
                event_index(queue.observation(),LoadEventKind::WorkStart,second+1));
    }
    {
        NativeLoadQueue queue(64);
        Signal entered,release;
        const auto failed_id=queue.enqueue([&]{entered.notify();release.wait();throw LoadFailure("pause race");},{});
        queue.enqueue([]{throw std::runtime_error("must remain queued");});
        queue.resume();entered.wait();
        std::atomic<bool> caught=false;
        std::jthread pauser([&]{try{queue.pause_and_wait();}catch(const LoadFailure&){caught=true;}});
        wait_for_event(queue,LoadEventKind::PauseRequest);
        release.notify();pauser.join();
        require(caught&&queue.pending()==2);
        const auto failed=queue.observation();
        require(event_index(failed,LoadEventKind::WorkFailure,failed_id)>0);
        for(const auto& event:failed.events)require(event.kind!=LoadEventKind::PauseAcknowledged&&
                                                    event.kind!=LoadEventKind::WorkComplete);
        rejects([&]{queue.resume();});
        rejects([&]{queue.enqueue([]{});});
    }
}

void pause_shutdown_and_thread_ownership() {
    using namespace okage;
    {
        auto queue=std::make_unique<NativeLoadQueue>(64);
        auto* const raw_queue=queue.get();
        Signal entered,parked;
        std::atomic<bool> checkpoint_result=true,queued_ran=false;
        LoadObservation at_shutdown;
        const auto running=queue->enqueue([&]{
            entered.notify();
            wait_for_event(*raw_queue,LoadEventKind::PauseRequest);
            checkpoint_result=raw_queue->checkpoint();
            if(!checkpoint_result)at_shutdown=raw_queue->observation();
            parked.notify();
        },{});
        queue->enqueue([&]{queued_ran=true;});
        queue->resume();entered.wait();
        queue->pause_and_wait();
        const auto before=queue->observation();
        require(event_index(before,LoadEventKind::PauseAcknowledged)>0&&queue->pending()==2);
        std::jthread destroyer([&]{queue.reset();});
        parked.wait();destroyer.join();
        require(!checkpoint_result&&!queued_ran);
        for(const auto& event:at_shutdown.events) {
            require(event.kind!=LoadEventKind::WorkComplete);
            if(event.kind==LoadEventKind::WorkStart)require(event.job==running);
        }
    }
    {
        NativeLoadQueue queue;
        bool checkpoint_rejected=false;
        queue.pause_and_wait();
        try{(void)queue.checkpoint();}catch(const std::logic_error&){checkpoint_rejected=true;}
        require(checkpoint_rejected);
        queue.resume();
    }
    {
        NativeLoadQueue queue;
        Signal entered,finished;
        std::atomic<bool> rejected=false;
        queue.enqueue([&]{
            entered.notify();
            try{queue.pause_and_wait();}catch(const std::logic_error&){rejected=true;}
            require(queue.checkpoint());
            finished.notify();
        });
        queue.resume();entered.wait();finished.wait();
        require(rejected);queue.check();
    }
}

void observed_lifecycle() {
    using namespace okage;
    NativeLoadQueue queue(256);
    Signal prepared,release,finished;
    std::uint64_t first=0;
    const LoadJobIdentity identity{14,118},second_identity{15,222};
    first=queue.enqueue([&]{
        queue.observe(LoadEventKind::Prepared,first,identity);
        prepared.notify();release.wait();
    },identity);
    const auto second=queue.enqueue([done=std::make_shared<OnRelease>(finished)]{},second_identity);
    require(first!=0&&second==first+1);
    auto before=queue.observation();require(before.events.size()==2);
    require(before.events[0].pending==1&&before.events[1].pending==2);
    queue.resume();prepared.wait();
    const auto held=queue.observation();
    require(held.events.size()==5&&held.dropped==0);
    const auto& marker=held.events.back();
    require(marker.kind==LoadEventKind::Prepared&&marker.job==first&&marker.identity==identity);
    require(marker.pending==2&&marker.running_job==first&&!marker.logical_tick);
    require(marker.thread!=std::this_thread::get_id());
    std::jthread reader([&]{
        for(unsigned i=0;i<100;++i){
            const auto snapshot=queue.observation();
            for(std::size_t j=0;j<snapshot.events.size();++j)require(snapshot.events[j].sequence==j+1);
        }
    });
    for(unsigned i=0;i<100;++i)queue.observe(LoadEventKind::Poll,first,identity,1000+i);
    reader.join();
    release.notify();finished.wait();queue.check();
    require(queue.pending()==0);
    queue.observe(LoadEventKind::MainContinuation,first,identity,1100);
    auto all=queue.observation();
    require(all.dropped==0&&before.events.size()==2&&held.events.size()==5);
    std::vector<LoadEventKind> first_kinds,second_kinds;
    for(const auto& e:all.events) {
        if(e.job==first&&e.kind!=LoadEventKind::Poll)first_kinds.push_back(e.kind);
        if(e.job==second)second_kinds.push_back(e.kind);
        if(e.kind==LoadEventKind::WorkComplete)
            require(e.running_job==0&&e.pending==(e.job==first?1u:0u)&&!e.failed);
        if(e.kind==LoadEventKind::WorkStart)require(e.running_job==e.job&&e.resumed);
    }
    require(first_kinds==std::vector{LoadEventKind::Enqueue,LoadEventKind::WorkStart,
        LoadEventKind::Prepared,LoadEventKind::WorkComplete,LoadEventKind::MainContinuation});
    require(second_kinds==std::vector{LoadEventKind::Enqueue,LoadEventKind::WorkStart,LoadEventKind::WorkComplete});
    require(all.events.back().logical_tick==1100&&all.events.back().thread==std::this_thread::get_id());
    all.events.front().identity={99,99};
    require(queue.observation().events.front().identity==identity);
    bool rejected=false;
    try{queue.observe(LoadEventKind::WorkComplete,first,identity);}catch(const std::invalid_argument&){rejected=true;}
    require(rejected);
    rejected=false;
    try{queue.observe(LoadEventKind::Prepared,second+1,identity);}catch(const std::invalid_argument&){rejected=true;}
    require(rejected);
}

void observed_failure_and_saturation() {
    using namespace okage;
    {
        NativeLoadQueue queue(32);Signal failed;
        const LoadJobIdentity identity{1,2};
        const auto id=queue.enqueue([done=std::make_shared<OnRelease>(failed)]{
            throw LoadFailure("observed failure");
        },identity);
        queue.enqueue([]{throw std::runtime_error("must not execute");});
        queue.resume();failed.wait();
        rejects([&]{queue.check();});require(queue.pending()==2);
        auto snapshot=queue.observation();unsigned failures=0;
        for(const auto& e:snapshot.events){
            require(e.kind!=LoadEventKind::WorkComplete);
            if(e.kind==LoadEventKind::WorkFailure){
                ++failures;require(e.job==id&&e.identity==identity&&e.failed&&e.pending==2&&e.running_job==0);
            }
        }
        require(failures==1);
    }
    {
        NativeLoadQueue queue(2);Signal done;
        queue.enqueue([finished=std::make_shared<OnRelease>(done)]{});
        queue.resume();done.wait();queue.check();require(queue.pending()==0);
        const auto snapshot=queue.observation();
        require(snapshot.events.size()==2&&snapshot.dropped==3); // start, completion, pending poll.
        require(snapshot.events[0].kind==LoadEventKind::Enqueue&&snapshot.events[1].kind==LoadEventKind::Resume);
    }
}
}
int main() {
    try {
        observed_lifecycle();observed_failure_and_saturation();
        cooperative_pause_and_checkpoint();
        pause_completion_and_failure_races();
        pause_shutdown_and_thread_ownership();
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
            require(queue.observation().events.empty()&&queue.observation().dropped==0);
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
            Signal started,release;
            std::atomic<bool> callback_finished=false,destructor_finished=false;
            auto owner=std::make_shared<unsigned>(0);
            auto queue=std::make_unique<okage::NativeLoadQueue>();
            queue->enqueue([&,owner]{started.notify();release.wait();*owner=73;callback_finished=true;});
            queue->resume();started.wait();
            std::barrier begin_destruction(2);
            std::jthread destroyer([&]{
                begin_destruction.arrive_and_wait();
                queue.reset();
                require(callback_finished);destructor_finished=true;
            });
            begin_destruction.arrive_and_wait();
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
