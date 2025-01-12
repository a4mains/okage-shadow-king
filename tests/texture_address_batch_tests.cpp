#include "texture_address_batch_cases.hpp"

#include <exception>
#include <iostream>
#include <limits>
#include <string_view>

namespace {
template<class Exception, class Action>
void rejects(Action action, const char* label) {
    try { action(); }
    catch (const Exception&) { return; }
    throw std::runtime_error(std::string(label) + ": expected rejection");
}

void validation_tests() {
    using namespace okage;
    validate_texture_address_batch(0, 0, 0);
    validate_texture_address_batch(1, 1, 1);
    validate_texture_address_batch(texture_address_batch_limit, texture_address_batch_limit);
    rejects<std::invalid_argument>([] { validate_texture_address_batch(1, 0); }, "short output");
    rejects<std::length_error>([] { validate_texture_address_batch(2, 2, 1); }, "short capacity");
    rejects<std::length_error>([] { validate_texture_address_batch(texture_address_batch_limit + 1,
        texture_address_batch_limit + 1); }, "count limit");
    rejects<std::length_error>([] { validate_texture_address_batch(0, 0,
        texture_address_batch_limit + 1); }, "capacity limit");
    constexpr auto max = std::numeric_limits<std::size_t>::max();
    rejects<std::length_error>([] { validate_texture_address_batch(max, max, max); }, "all size overflow");
    rejects<std::length_error>([] { validate_texture_address_batch(max / sizeof(TextureAddressQuery) + 1, max); }, "input byte overflow");
    rejects<std::length_error>([] { validate_texture_address_batch(0, max / sizeof(TextureAddressResult) + 1); }, "output byte overflow");
    rejects<std::length_error>([] { validate_texture_address_batch(0, 0, max / sizeof(TextureAddressQuery) + 1); }, "capacity byte overflow");

    std::vector<TextureAddressQuery> oversized(texture_address_batch_limit + 1, {0, 0, 0, 0, 0});
    constexpr TextureAddressResult poison{0xdeadbeefu, 0xcafebabeu};
    std::vector<TextureAddressResult> output(oversized.size(), poison);
    for (auto execute : {texture_address_batch_cpu, texture_address_batch_scalar}) {
        rejects<std::length_error>([&] { execute(oversized, output); }, "oversized execution");
        rejects<std::invalid_argument>([&] {
            execute(std::span<const TextureAddressQuery>(oversized).first(2),
                    std::span<TextureAddressResult>(output).first(1));
        }, "short execution output");
        for (const auto& result : output)
            okage::test::require(result == poison, "rejection wrote output");
        execute({}, {});
    }
}

void known_address_tests() {
    using namespace okage;
    const std::array<TextureAddressQuery, 8> input = {{{0,16383,1,0,0}, {0,16383,1,8,0},
        {19,16383,1,16,0}, {19,0,3,0,64}, {19,0,3,0,128},
        {19,0,0,0,128}, {0,0,0,0,32}, {1,0,0,0,0}}};
    const std::array<TextureAddressResult, 8> expected = {{{0x3fff00,1}, {0,1}, {0,1},
        {8192,1}, {24576,1}, {0,1}, {0,1}, {0,0}}};
    std::array<TextureAddressResult, 8> actual{};
    for (auto execute : {texture_address_batch_cpu, texture_address_batch_scalar}) {
        execute(input, actual);
        okage::test::require(actual == expected, "known wrap/odd/zero-stride address failed");
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        bool metal = false;
        if (argc == 2 && std::string_view(argv[1]) == "--metal") metal = true;
        else if (argc != 1) throw std::invalid_argument("usage: texture_address_batch_tests [--metal]");
        validation_tests();
        known_address_tests();
        auto checked = okage::test::check_texture_address_batch(okage::texture_address_batch_scalar, "scalar");
        checked += okage::test::check_texture_address_batch(okage::texture_address_batch_cpu, "cpu");
        if (metal) {
#if defined(OKAGE_TEST_METAL) && OKAGE_TEST_METAL
            auto backend = okage::make_metal_texture_address_batch(okage::texture_address_batch_limit, true);
            checked += okage::test::check_texture_address_batch(
                [&](auto input, auto output) { backend->execute(input, output); }, "metal");
            constexpr okage::TextureAddressResult poison{0xdeadbeefu, 0xcafebabeu};
            std::array<okage::TextureAddressQuery, 2> input{{{0,0,0,0,0}, {19,1,3,127,64}}};
            std::array<okage::TextureAddressResult, 1> output{poison};
            rejects<std::invalid_argument>([&] { backend->execute(input, output); }, "metal short output");
            okage::test::require(output[0] == poison, "metal rejection wrote output");
            auto small = okage::make_metal_texture_address_batch(1, true);
            std::array<okage::TextureAddressResult, 2> pair{poison, poison};
            rejects<std::length_error>([&] { small->execute(input, pair); }, "metal short capacity");
            okage::test::require(pair[0] == poison && pair[1] == poison, "metal capacity rejection wrote output");
#else
            throw std::runtime_error("--metal requires an OKAGE_TEST_METAL build; no CPU fallback");
#endif
        }
        std::cout << "texture address batch tests passed: " << checked << " exact query checks"
                  << (metal ? " (CPU + Metal)\n" : " (CPU)\n");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "texture address batch tests failed: " << error.what() << '\n';
        return 1;
    }
}
