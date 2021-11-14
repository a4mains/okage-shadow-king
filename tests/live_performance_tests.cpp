#include "okage/live_performance.hpp"
#include "okage/live_scheduler.hpp"
#include "okage/audio_ring.hpp"
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <vector>
#include <unistd.h>

namespace fs=std::filesystem;
void check(bool value,unsigned where=__builtin_LINE()) {
    if(!value)throw std::runtime_error("Performance test failed at line "+std::to_string(where));
}
template<class F> void rejects(F f) {
    bool threw=false;try {f();}catch(const std::runtime_error&){threw=true;}check(threw);
}
double number(const std::string& json,const std::string& key) {
    const auto at=json.find('"'+key+"\":");check(at!=std::string::npos);
    return std::stod(json.substr(at+key.size()+3));
}
std::vector<std::string> lines(const fs::path& path) {
    std::ifstream in(path);check(bool(in));std::vector<std::string> result;std::string line;
    while(std::getline(in,line))result.push_back(line);return result;
}
void interval_checks() {
    okage::PerformanceInterval interval;
    rejects([&]{interval.draw(0);});
    rejects([&]{interval.begin(std::numeric_limits<double>::infinity(),0);});
    interval.begin(100,40);
    rejects([&]{interval.begin(100,40);});
    rejects([&]{interval.tick(100,100.1,true);});
    rejects([&]{interval.update_begin(99,0);});
    rejects([&]{interval.update_begin(100,-1);});
    interval.update_begin(100,0);
    rejects([&]{interval.update_begin(100,0);});
    rejects([&]{interval.finish(100,40,okage::PerformanceEnd::pause,1);});
    interval.tick(100.0625,100.125,true);
    interval.tick(100.125,100.25,true);
    interval.draw(100.3125);
    interval.refresh(100.25,100.375);
    interval.update_end(100.4375);
    interval.draw(100.46875);
    interval.update_begin(100.5,.25);
    interval.tick(100.5625,100.625,false);
    interval.refresh(100.625,100.75);
    interval.update_end(100.875);
    interval.discard_elapsed(.125); // Cap applied at the pause boundary.
    const auto json=interval.finish(101,43,okage::PerformanceEnd::pause,1);
    check(!interval.active());
    check(number(json,"wall_duration_s")==1);
    check(number(json,"completed_simulation_ticks")==3);
    check(number(json,"simulation_ticks_per_wall_second")==3);
    check(number(json,"rendered_ticks")==2);
    check(number(json,"update_callbacks")==2);
    check(number(json,"appkit_draw_callbacks")==2);
    check(number(json,"appkit_draw_callbacks_per_wall_second")==2);
    check(number(json,"elapsed_discarded_by_update_cap_s")==.375);
    check(json.find("\"update_callback_gaps\":{\"count\":1,\"sum_ms\":500")!=json.npos);
    check(json.find("\"update_wall\":{\"count\":2,\"sum_ms\":812.5")!=json.npos);
    check(json.find("\"completed_tick_wall\":{\"count\":3,\"sum_ms\":250")!=json.npos);
    check(json.find("\"appkit_draw_callback_gaps\":{\"count\":1,\"sum_ms\":156.25")!=json.npos);
    check(json.find("\"frame_counter_matches_completed_ticks\":true")!=json.npos);
    interval.begin(200,43);
    const auto empty=interval.finish(200,43,okage::PerformanceEnd::window_close,2);
    check(number(empty,"completed_simulation_ticks")==0 && number(empty,"update_callbacks")==0);
    check(empty.find("\"simulation_ticks_per_wall_second\":null")!=empty.npos);
    check(empty.find("\"appkit_draw_callback_gaps\":{\"count\":0")!=empty.npos);
    interval.begin(300,43);interval.update_begin(300,0);interval.update_end(300.25);
    const auto failed=interval.finish(300.25,44,okage::PerformanceEnd::runtime_error,3);
    check(number(failed,"completed_simulation_ticks")==0 && number(failed,"frame_counter_delta")==1);
    check(failed.find("\"frame_counter_matches_completed_ticks\":false")!=failed.npos);
    interval.begin(400,44);interval.update_begin(400,0);interval.tick(400,400.125,true);interval.update_end(400.25);
    rejects([&]{interval.finish(400.25,44,okage::PerformanceEnd::pause,4);});
}
void scheduler_checks() {
    okage::LiveTickClock clock;
    rejects([&]{clock.admit(-1);});
    rejects([&]{clock.admit(std::numeric_limits<double>::infinity());});
    rejects([&]{clock.complete_tick();});
    unsigned ticks=0;
    // At 120 callbacks/s, an otherwise fast simulation still advances 60 ticks/s.
    for(unsigned i=0;i<120;++i) {
        check(clock.admit(1./120)==1./120);
        if(clock.tick_due()){clock.complete_tick();++ticks;}
        check(!clock.tick_due());
    }
    check(ticks==60 && std::abs(clock.pending())<1e-12);
    check(clock.admit(.6)==.25);check(clock.tick_due());clock.complete_tick();
    check(!clock.tick_due());rejects([&]{clock.complete_tick();});
    check(std::abs(clock.pending()-(.25-1./60))<1e-12);
    for(unsigned i=0;i<10;++i){clock.admit(.025);clock.complete_tick();check(!clock.tick_due());}
    check(std::abs(clock.pending()-(.5-11./60))<1e-12);
    // Later callbacks may pay retained debt even with no new wall admission.
    const auto debt=clock.pending();clock.admit(0);clock.complete_tick();
    check(std::abs(clock.pending()-(debt-1./60))<1e-12);
    clock.reset();check(!clock.tick_due() && clock.pending()==0);
    clock.admit(1./120);check(!clock.tick_due()); // no pre-pause debt carried over
}
void audio_ring_checks() {
    okage::AudioPcmRing ring;
    std::array<std::int16_t,1600> out;
    out.fill(123);ring.refill(out);
    check(std::all_of(out.begin(),out.end(),[](auto x){return x==0;}));
    auto stats=ring.stats();check(stats.refills==1 && stats.starved_refills==1);
    check(stats.silence_frames_enqueued==800 && stats.source_frames_enqueued==0);
    std::vector<std::int16_t> partial(1000);
    for(unsigned i=0;i<500;++i){partial[i*2]=i+1;partial[i*2+1]=-int(i+1);}
    ring.submit(partial);ring.refill(out);
    check(std::equal(partial.begin(),partial.end(),out.begin()));
    check(std::all_of(out.begin()+1000,out.end(),[](auto x){return x==0;}));
    stats=ring.stats();check(stats.submitted_frames==500 && stats.ring_frames==0);
    check(stats.source_frames_enqueued==500 && stats.silence_frames_enqueued==1100);
    check(stats.refills==2 && stats.starved_refills==2);
    std::vector<std::int16_t> full(okage::AudioPcmRing::capacity*2);
    for(unsigned i=0;i<okage::AudioPcmRing::capacity;++i){full[i*2]=i;full[i*2+1]=-int(i);}
    ring.submit(full);
    std::array<std::int16_t,2> extra{321,-321};
    rejects([&]{ring.submit(extra);});
    stats=ring.stats();check(stats.producer_overruns==1 && stats.ring_frames==okage::AudioPcmRing::capacity);
    check(stats.submitted_frames==500+okage::AudioPcmRing::capacity);
    for(unsigned base=0;base<okage::AudioPcmRing::capacity;base+=800){
        ring.refill(out);const auto count=std::min(800u,okage::AudioPcmRing::capacity-base);
        check(std::equal(full.begin()+base*2,full.begin()+(base+count)*2,out.begin()));
        check(std::all_of(out.begin()+count*2,out.end(),[](auto x){return x==0;}));
    }
    ring.submit(extra);ring.refill(out);check(out[0]==321 && out[1]==-321 && out[2]==0);
    stats=ring.stats();check(stats.ring_frames==0);
    check(stats.submitted_frames==stats.source_frames_enqueued);
    check(stats.refills*800==stats.source_frames_enqueued+stats.silence_frames_enqueued);
    check(stats.producer_overruns==1);
    okage::PerformanceInterval report;report.begin(1,0);report.audio_sample(stats);
    const auto failed=report.finish(1,0,okage::PerformanceEnd::runtime_error,1);
    check(number(failed,"audio_producer_overruns")==1);
    check(number(failed,"audio_source_frames_enqueued")==stats.source_frames_enqueued);
    check(number(failed,"audio_silence_frames_enqueued")==stats.silence_frames_enqueued);
}
void scheduler_audio_interval_checks() {
    okage::PerformanceInterval interval;okage::LiveTickClock clock;
    okage::AudioPcmRing ring;std::array<std::int16_t,1600> pcm{},out{};
    interval.begin(10,100);
    const auto first=clock.admit(.6);interval.scheduler_admit(first,clock.pending());
    interval.update_begin(10.6,.6-first);
    ring.submit(pcm);ring.refill(out);ring.refill(out); // one real block, one silent block
    clock.complete_tick();interval.tick(10.6,10.625,true);
    interval.scheduler_pending(clock.pending());interval.audio_sample(ring.stats());interval.update_end(10.625);
    const auto second=clock.admit(.03);interval.scheduler_admit(second,clock.pending());
    interval.update_begin(10.63,0);ring.submit(pcm);clock.complete_tick();
    interval.tick(10.63,10.655,true);interval.scheduler_pending(clock.pending());
    interval.audio_sample(ring.stats());interval.update_end(10.655);
    const auto final_gap=clock.admit(.07);interval.scheduler_admit(final_gap,clock.pending());
    interval.audio_sample(ring.stats());clock.reset();
    const auto json=interval.finish(10.7,102,okage::PerformanceEnd::pause,1);
    check(number(json,"max_ticks_per_update")==1 && number(json,"updates_with_pending_ticks")==2);
    check(std::abs(number(json,"admitted_elapsed_s")-.35)<1e-12);
    check(std::abs(number(json,"elapsed_discarded_by_update_cap_s")-.35)<1e-12);
    check(std::abs(number(json,"accumulator_accounting_error_s"))<1e-12);
    check(std::abs(number(json,"accumulator_pending_end_s")-(.35-2./60))<1e-12);
    check(number(json,"accumulator_pending_end_s")==number(json,"accumulator_discarded_on_pause_s"));
    check(number(json,"audio_submitted_source_frames")==1600 && number(json,"audio_ring_frames_end")==800);
    check(number(json,"audio_source_frames_enqueued")==800 && number(json,"audio_silence_frames_enqueued")==800);
    check(number(json,"audio_refills")==2 && number(json,"audio_starved_refills")==1);
    interval.begin(20,102);const auto resumed_gap=clock.admit(.01);interval.scheduler_admit(resumed_gap,clock.pending());
    okage::AudioOutputStats new_output;interval.audio_sample(new_output);
    const auto resumed=interval.finish(20.01,102,okage::PerformanceEnd::window_close,2);
    check(number(resumed,"audio_silence_frames_enqueued")==0 && number(resumed,"audio_submitted_source_frames")==0);
    check(number(resumed,"accumulator_pending_end_s")==.01);
    check(number(resumed,"accumulator_discarded_on_pause_s")==0);
    check(number(resumed,"accumulator_accounting_error_s")==0);
}
void distribution_checks() {
    okage::PerformanceDistribution distribution;
    rejects([&]{distribution.add(-1);});
    rejects([&]{distribution.add(std::numeric_limits<double>::quiet_NaN());});
    distribution.add(0);distribution.add(.001);distribution.add(61);
    check(distribution.count==3 && distribution.min_seconds==0 && distribution.max_seconds==61);
    check(distribution.buckets[0]==1 && distribution.buckets[2]==1 && distribution.buckets.back()==1);
    check(std::accumulate(distribution.buckets.begin(),distribution.buckets.end(),std::uint64_t{})==3);
    check(distribution.json().find("\"sum_ms\":61001")!=std::string::npos);
}
void output_checks(const fs::path& directory) {
    const auto output=directory/"report.jsonl";
    {
        okage::LivePerformance report(output);
        const auto metadata=lines(output);check(metadata.size()==1);
        check(metadata[0].find("\"type\":\"metadata\"")!=std::string::npos);
        rejects([&]{okage::LivePerformance duplicate(output);});
        check(lines(output)==metadata);
        report.interval.begin(1,20);report.interval.update_begin(1,0);
        report.interval.tick(1,1.125,true);report.interval.update_end(1.25);
        report.finish(2,21,okage::PerformanceEnd::pause);
        const auto first=lines(output);check(first.size()==2);
        report.finish(3,21,okage::PerformanceEnd::window_close);check(lines(output)==first);
        report.interval.begin(4,21);report.finish(5,21,okage::PerformanceEnd::window_close);
        const auto second=lines(output);check(second.size()==3 && second[1]==first[1]);
        check(number(second[2],"interval")==2 && number(second[2],"completed_simulation_ticks")==0);
    }
    check(lines(output).size()==3);
    rejects([&]{okage::LivePerformance invalid(directory);});
    const auto link=directory/"link.jsonl";fs::create_symlink(directory/"absent",link);
    rejects([&]{okage::LivePerformance invalid(link);});check(fs::is_symlink(link));
    rejects([&]{okage::LivePerformance invalid(directory/"missing-parent"/"report.jsonl");});
}
int main() {
    fs::path directory;
    try {
        interval_checks();distribution_checks();scheduler_checks();audio_ring_checks();scheduler_audio_interval_checks();
        auto pattern=(fs::temp_directory_path()/"okage-performance-XXXXXX").string();
        const auto created=::mkdtemp(pattern.data());check(created);directory=created;
        output_checks(directory);fs::remove_all(directory);
        std::cout<<"Live fairness/debt, PCM ring ordering/starvation/overflow, interval counters and exclusive reports passed\n";
    }catch(const std::exception& e) {
        if(!directory.empty())fs::remove_all(directory);
        std::cerr<<e.what()<<'\n';return 1;
    }
}
