#include "okage/scene_renderer.hpp"
#include <array>
#include <barrier>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>

int main() {
    try {
        // Standalone process: no scene construction, owned assets or commands.
        if(okage::SceneRenderer::cached_backend_count()!=0)
            throw std::runtime_error("Scene backend unexpectedly prepared before use");
        constexpr std::size_t count=8;
        std::barrier start(static_cast<std::ptrdiff_t>(count));
        std::array<std::exception_ptr,count> errors{};
        {
            std::array<std::jthread,count> workers;
            for(std::size_t i=0;i<count;++i)workers[i]=std::jthread([&,i] {
                start.arrive_and_wait();
                try { okage::SceneRenderer::prepare_backend(); }
                catch(...) { errors[i]=std::current_exception(); }
            });
        }
        for(const auto& error:errors)if(error)std::rethrow_exception(error);
        if(okage::SceneRenderer::cached_backend_count()!=1)
            throw std::runtime_error("Concurrent preparation did not share one device bundle");
        for(unsigned i=0;i<4;++i)okage::SceneRenderer::prepare_backend();
        if(okage::SceneRenderer::cached_backend_count()!=1)
            throw std::runtime_error("Repeated preparation published another bundle");
        std::cout << "Scene backend cache tests passed\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
