#include "okage/live_performance.hpp"
#include "okage/live_scheduler.hpp"
#include "okage/audio_ring.hpp"
#include "okage/native_run.hpp"
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <source_location>
#include <stdexcept>
#include <vector>
#include <unistd.h>

namespace fs=std::filesystem;
void check(bool value,const std::source_location where=std::source_location::current()) {
    if(!value)throw std::runtime_error("Performance test failed at line "+std::to_string(where.line()));
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
void deferred_tick_checks() {
    okage::PerformanceInterval interval;
    rejects([&]{interval.deferred_tick(1,1,true);});

    interval.begin(10,20);
    interval.update_begin(10.125,0);
    interval.deferred_tick(10.0625,10.25,true);
    interval.update_end(10.3125);
    const auto cross_callback=interval.finish(10.375,21,okage::PerformanceEnd::pause,1);
    check(number(cross_callback,"completed_simulation_ticks")==1);
    check(number(cross_callback,"rendered_ticks")==1);
    check(number(cross_callback,"max_ticks_per_update")==1);
    check(cross_callback.find("\"completed_tick_wall\":{\"count\":1,\"sum_ms\":187.5")!=cross_callback.npos);

    interval.begin(20,30);
    interval.update_begin(20.125,0);
    interval.update_end(20.1875);
    interval.deferred_tick(20.0625,20.25,false);
    const auto pause_boundary=interval.finish(20.3125,31,okage::PerformanceEnd::pause,2);
    check(number(pause_boundary,"completed_simulation_ticks")==1);
    check(number(pause_boundary,"rendered_ticks")==0);
    check(number(pause_boundary,"update_callbacks")==1);
    check(number(pause_boundary,"max_ticks_per_update")==0);

    interval.begin(40,50);
    rejects([&]{interval.deferred_tick(40,std::numeric_limits<double>::infinity(),false);});
    rejects([&]{interval.deferred_tick(std::numeric_limits<double>::quiet_NaN(),40,false);});
    rejects([&]{interval.deferred_tick(39.999,40,false);});
    rejects([&]{interval.deferred_tick(40,39.999,false);});
    interval.update_begin(40.125,0);
    rejects([&]{interval.deferred_tick(40.0625,40.0625,false);});
    interval.update_end(40.25);
    interval.finish(40.25,50,okage::PerformanceEnd::window_close,3);
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
void render_range_checks() {
    using okage::NativeRenderRange;
    for(const auto text:{"", "1", ":2", "1:", "0:2", "1:0", "2:1", "-1:2", "+1:2",
                        " 1:2", "1:2 ", "1:2:3", "1:2x", "4294967296:4294967296"})
        rejects([&]{okage::native_render_range(text);});
    const auto single=okage::native_render_range("7:7");check(single.first==7&&single.last==7);
    // Unordered, overlapping repeated options form a union, never extra ticks.
    const std::array ranges{okage::native_render_range("91115:91595"),single,
                           okage::native_render_range("91116:91594")};
    okage::validate_native_render_ranges(ranges,91595);
    rejects([&]{okage::validate_native_render_ranges(ranges,91594);});
    rejects([&]{okage::validate_native_render_ranges(ranges,0);});
    okage::validate_native_render_ranges({},0);
    for(const auto bad:{NativeRenderRange{0,1},NativeRenderRange{2,1}})
        rejects([&]{okage::validate_native_render_ranges(std::span(&bad,1),91595);});
    unsigned selected=0;
    for(unsigned frame=0;frame<=91596;++frame) {
        const bool expected=frame==7||(frame>=11310&&frame<=11542)||(frame>=91115&&frame<=91595);
        const bool actual=okage::native_render_tick(frame,11310,11542,ranges);
        check(actual==expected);selected+=actual;
        check(okage::native_render_tick(frame,1,0,{})==(frame>=1));
        check(!okage::native_render_tick(frame,0,0,{}));
    }
    check(selected==1+233+481);
    check(okage::native_render_tick(91596,91115,0,ranges));
    check(!okage::native_render_tick(91596,0,0,ranges));
}
void run_plan_checks(const fs::path& directory) {
    const auto capture=directory/"end.png";
    const auto plain=okage::native_run_plan(0,0,0,{},100);
    check(!plain.starts_paused&&!plain.bounded_handoff&&plain.replay_frames==0);
    const auto headless=okage::native_run_plan(20,0,0,capture,100);
    check(headless.replay_frames==20&&headless.replay_capture==capture&&headless.live_capture.empty());
    const auto preview=okage::native_run_plan(0,20,0,capture,100);
    check(preview.starts_paused&&!preview.bounded_handoff&&preview.replay_capture==capture);
    const auto ui=okage::native_run_plan(0,0,40,capture,100);
    check(ui.replay_frames==0&&!ui.starts_paused&&ui.live_capture==capture);
    const auto combined=okage::native_run_plan(0,20,40,capture,100);
    check(combined.replay_frames==20&&!combined.starts_paused&&combined.bounded_handoff);
    check(combined.replay_capture==fs::path(capture.string()+".preview.png")&&combined.live_capture==capture);
    const std::array<unsigned,2> prelude_ticks{1,20},live_ticks{1,21};
    okage::validate_native_capture_selection(combined,prelude_ticks);
    rejects([&]{okage::validate_native_capture_selection(combined,live_ticks);});
    rejects([&]{okage::validate_native_capture_selection(ui,prelude_ticks);});
    rejects([&]{okage::validate_native_capture_selection(okage::native_run_plan(0,20,0,{},100),prelude_ticks);});
    okage::validate_native_capture_selection(plain,{});
    for(unsigned end:{1u,19u,20u,101u})rejects([&]{okage::native_run_plan(0,20,end,capture,100);});
    rejects([&]{okage::native_run_plan(1,20,40,capture,100);});
    rejects([&]{okage::native_run_plan(1,0,40,capture,100);});
    rejects([&]{okage::native_run_plan(1,20,0,capture,100);});
    rejects([&]{okage::native_run_plan(101,0,0,capture,100);});
    rejects([&]{okage::native_run_plan(0,101,0,capture,100);});
    rejects([&]{okage::native_run_plan(0,20,40,{},100);});
    rejects([&]{okage::native_run_plan(0,0,40,{},100);});
    check(okage::native_run_plan(0,99,100,capture,100).bounded_handoff);
    okage::require_fresh_native_capture_family(capture);
    rejects([&]{okage::require_fresh_native_capture_family(directory/"absent"/"end.png");});
    const auto blocked_parent=directory/"not-a-directory";
    {std::ofstream out(blocked_parent);out<<"retained";}
    rejects([&]{okage::require_fresh_native_capture_family(blocked_parent/"end.png");});
    check(lines(blocked_parent)==std::vector<std::string>{"retained"});fs::remove(blocked_parent);
    for(const auto& name:{"end.png","end.png.preview.png","end.png.audio.wav","end.png.frames/tick-1.png"})
        check(okage::native_capture_family_contains(capture,directory/name));
    check(!okage::native_capture_family_contains(capture,directory/"end.performance.jsonl"));
    check(!okage::native_capture_family_contains(capture,directory/"other"/"end.png"));
    const auto sidecar=directory/"end.png.preview.png.json";
    {std::ofstream out(sidecar);out<<"retained";}
    rejects([&]{okage::require_fresh_native_capture_family(capture);});
    check(lines(sidecar)==std::vector<std::string>{"retained"});fs::remove(sidecar);
    fs::create_directory(directory/"end.png.frames");
    rejects([&]{okage::require_fresh_native_capture_family(capture);});fs::remove(directory/"end.png.frames");
    fs::create_symlink(directory/"missing",capture);
    rejects([&]{okage::require_fresh_native_capture_family(capture);});check(fs::is_symlink(capture));fs::remove(capture);
    okage::require_fresh_native_capture_family(capture);
}
void bounded_interval_checks(const fs::path& directory) {
    const auto path=directory/"bounded.jsonl";
    rejects([&]{okage::LivePerformance invalid(path,okage::PerformanceTestRange{20,20});});
    check(!fs::exists(path));
    okage::LivePerformance report(path,okage::PerformanceTestRange{20,22});
    const auto metadata=lines(path).at(0);
    check(metadata.find("\"bounded_ui_test\":true")!=metadata.npos);
    check(number(metadata,"headless_prelude_end_frame")==20&&number(metadata,"ui_test_target_ticks")==2);
    report.interval.begin(10,20);
    for(unsigned i=0;i<2;++i) {
        const double now=10+i*.125;
        report.interval.update_begin(now,0);report.interval.tick(now,now+.03125,true);
        report.interval.refresh(now+.03125,now+.0625);report.interval.update_end(now+.0625);
    }
    rejects([&]{report.finish(10.25,21,okage::PerformanceEnd::test_complete);});
    report.finish(10.25,22,okage::PerformanceEnd::test_complete);
    const auto completed=lines(path);check(completed.size()==2);
    check(completed[1].find("\"end_reason\":\"test_complete\"")!=completed[1].npos);
    check(number(completed[1],"completed_simulation_ticks")==2&&number(completed[1],"frame_counter_delta")==2);
    check(number(completed[1],"update_callbacks")==2&&number(completed[1],"wall_duration_s")==.25);
    rejects([&]{report.interval.draw(99);});
    report.finish(100,22,okage::PerformanceEnd::application_terminate);
    check(lines(path)==completed);
    okage::PerformanceInterval missed;missed.begin(1,20);
    rejects([&]{missed.finish(2,22,okage::PerformanceEnd::test_complete,1);});
}
int main() {
    fs::path directory;
    try {
        render_range_checks();interval_checks();deferred_tick_checks();distribution_checks();scheduler_checks();audio_ring_checks();scheduler_audio_interval_checks();
        auto pattern=(fs::temp_directory_path()/"okage-performance-XXXXXX").string();
        const auto created=::mkdtemp(pattern.data());check(created);directory=created;
        output_checks(directory);run_plan_checks(directory);bounded_interval_checks(directory);fs::remove_all(directory);
        std::cout<<"Live fairness/debt, PCM ring ordering/starvation/overflow, interval counters and exclusive reports passed\n";
    }catch(const std::exception& e) {
        if(!directory.empty())fs::remove_all(directory);
        std::cerr<<e.what()<<'\n';return 1;
    }
}
