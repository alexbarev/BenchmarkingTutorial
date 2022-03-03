#include <algorithm> // `std::sort`
#include <cmath>     // `std::pow`
#include <cstdint>   // `int32_t`
#include <cstdlib>   // `std::rand`
#include <execution> // `std::execution::par_unseq`
#include <new>       // `std::launder`
#include <random>    // `std::mt19937`
#include <vector>    // `std::algorithm`

#include <benchmark/benchmark.h>

namespace bm = benchmark;

static void i32_addition(bm::State &state) {
    int32_t a = 0, b = 0, c = 0;
    for (auto _ : state)
        c = a + b;
}

// The compiler will just optimize everything out.
// After the first run, the value of `c` won't change.
// The benchmark will show 0ns per iteration.
BENCHMARK(i32_addition);

static void i32_addition_random(bm::State &state) {
    int32_t c = 0;
    for (auto _ : state)
        c = std::rand() + std::rand();
}

// This run in 25ns, or about 100 CPU cycles.
// Is integer addition really that expensive?
BENCHMARK(i32_addition_random);

static void i32_addition_random_and_used(bm::State &state) {
    int32_t a = std::rand(), b = std::rand(), c = 0;
    for (auto _ : state)
        bm::DoNotOptimize(c = (++a) + (++b));
}

// We trigger the two `inc` instructions and the `add` on x86.
// This shouldn't take more then 0.7 ns on a modern CPU.
// So all the time spent - was in the `rand()`!
BENCHMARK(i32_addition_random_and_used);

// Our `rand()` is 100 cycles on a single core, but it involves
// global state management, so it can be as slow 12'000 ns with
// just 8 threads.
BENCHMARK(i32_addition_random)->Threads(8);
BENCHMARK(i32_addition_random_and_used)->Threads(8);

// ------------------------------------
// ## Let's do some basic math
// ### Maclaurin series
// ------------------------------------

static void f64_sin_maclaurin(bm::State &state) {
    double argument = std::rand(), result = 0;
    for (auto _ : state) {
        result = argument - std::pow(argument, 3) / 6 + std::pow(argument, 5) / 120;
        bm::DoNotOptimize(result += argument += 1.0);
    }
}

// Lets compute the `sin(x)` via Maclaurin series.
// It will involve a fair share of floating point operations.
// We will only take the first 3 parts of the expansion:
//  sin(x) ~ x - (x^3) / 3! + (x^5) / 5!
// https://en.wikipedia.org/wiki/Taylor_series
BENCHMARK(f64_sin_maclaurin);

static void f64_sin_maclaurin_powless(bm::State &state) {
    double argument = std::rand(), result = 0;
    for (auto _ : state) {
        result = argument - (argument * argument * argument) / 6.0 +
                 (argument * argument * argument * argument * argument) / 120.0;
        bm::DoNotOptimize(result += argument += 1.0);
    }
}

// Help the compiler Help you!
// Instead of using the heavy generic operation - describe your special case to the compiler!
BENCHMARK(f64_sin_maclaurin_powless);

// The old syntax in GCC is: __attribute__((optimize("-ffast-math")))
[[gnu::optimize("-ffast-math")]] static void f64_sin_maclaurin_with_fast_math(bm::State &state) {
    double argument = std::rand(), result = 0;
    for (auto _ : state) {
        result = argument - (argument * argument * argument) / 6.0 +
                 (argument * argument * argument * argument * argument) / 120.0;
        bm::DoNotOptimize(result += argument += 1.0);
    }
}

// Floating point math is not associative!
// So it's not reorderable! And it requires extra annotation!
// Use only when you work with low-mid precision numbers and values of similar magnitude.
// As always with IEEE-754, you have same number of elements in [-inf,-1], [-1,0], [0,1], [1,+inf].
// https://en.wikipedia.org/wiki/Double-precision_floating-point_format
BENCHMARK(f64_sin_maclaurin_with_fast_math);

// ------------------------------------
// ## Lets look at Integer Division
// ### If floating point arithmetic can be fast, what about integer division?
// ------------------------------------

static void i64_division(bm::State &state) {
    int64_t a = std::rand(), b = std::rand(), c = 0;
    for (auto _ : state)
        bm::DoNotOptimize(c = (++a) / (++b));
}

// If we take 32-bit integers - their division can be performed via `double`
// without loss of accuracy. Result: 7ns, or 15x more expensive then addition.
BENCHMARK(i64_division);

static void i64_division_by_const(bm::State &state) {
    int64_t b = 2147483647;
    int64_t a = std::rand(), c = 0;
    for (auto _ : state)
        bm::DoNotOptimize(c = (++a) / *std::launder(&b));
}

// Let's fix a constant, but `std::launder` it a bit.
// So it looks like a generic pointer and not explicitly
// a constant as a developer might have seen.
// Result: more or less the same as before.
BENCHMARK(i64_division_by_const);

static void i64_division_by_constexpr(bm::State &state) {
    constexpr int64_t b = 2147483647;
    int64_t a = std::rand(), c = 0;
    for (auto _ : state)
        bm::DoNotOptimize(c = (++a) / b);
}

// But once we mark it as a `constexpr`, the compiler will replace
// heavy divisions with a combination of simpler shifts and multiplications.
// https://www.sciencedirect.com/science/article/pii/S2405844021015450
BENCHMARK(i64_division_by_constexpr);

// ------------------------------------
// ## Where else those tricks are needed
// ------------------------------------

[[gnu::target("default")]] static void u64_population_count(bm::State &state) {
    auto a = static_cast<uint64_t>(std::rand());
    for (auto _ : state)
        bm::DoNotOptimize(__builtin_popcount(++a));
}

BENCHMARK(u64_population_count)->MinTime(10);

[[gnu::target("popcnt")]] static void u64_population_count_x86(bm::State &state) {
    auto a = static_cast<uint64_t>(std::rand());
    for (auto _ : state)
        bm::DoNotOptimize(__builtin_popcount(++a));
}

BENCHMARK(u64_population_count_x86)->MinTime(10);

// ------------------------------------
// ## Enough with nano-second stuff!
// ### Lets do something bigger
// ------------------------------------

static void sorting(bm::State &state) {

    auto count = static_cast<size_t>(state.range(0));
    auto include_preprocessing = static_cast<bool>(state.range(1));

    std::vector<int32_t> array(count);
    std::iota(array.begin(), array.end(), 1);

    for (auto _ : state) {

        if (!include_preprocessing)
            state.PauseTiming();
        // Reverse order is the most classical worst case, but not the only one.
        std::reverse(array.begin(), array.end());
        if (!include_preprocessing)
            state.ResumeTiming();

        std::sort(array.begin(), array.end());
        bm::DoNotOptimize(array.size());
    }
}

// `std::sort` will invoke a modification of Quick-Sort.
// It's worst case complexity is ~O(N^2), but what the hell are those numbers??
BENCHMARK(sorting)->Args({3, false})->Args({3, true});
BENCHMARK(sorting)->Args({4, false})->Args({4, true});

static void upper_cost_of_branching(bm::State &state) {
    volatile int32_t a = std::rand();
    volatile int32_t c = 0;
    for (auto _ : state) {
        volatile bool prefer_addition = (a * 2147483647 ^ c) % 2 == 0;
        if (prefer_addition)
            c += ++a;
        else
            c -= ++a;
    }
}

BENCHMARK(upper_cost_of_branching);

static void upper_cost_of_pausing(bm::State &state) {
    int32_t a = std::rand(), c = 0;
    for (auto _ : state) {
        state.PauseTiming();
        ++a;
        state.ResumeTiming();
        bm::DoNotOptimize(c += a);
    }
}

BENCHMARK(upper_cost_of_pausing);

template <bool include_preprocessing_k> static void sorting_template(bm::State &state) {

    auto count = static_cast<size_t>(state.range(0));
    std::vector<int32_t> array(count);
    std::iota(array.begin(), array.end(), 1);

    for (auto _ : state) {

        if constexpr (!include_preprocessing_k)
            state.PauseTiming();
        std::reverse(array.begin(), array.end());
        if constexpr (!include_preprocessing_k)
            state.ResumeTiming();

        std::sort(array.begin(), array.end());
        bm::DoNotOptimize(array.size());
    }
}

// Now, our control-flow will not affect the measurements!
// "Don't pay what you don't use" becomes: "Don't pay for what you can avoid!"
BENCHMARK_TEMPLATE(sorting_template, false)->Arg(3);
BENCHMARK_TEMPLATE(sorting_template, true)->Arg(3);
BENCHMARK_TEMPLATE(sorting_template, false)->Arg(4);
BENCHMARK_TEMPLATE(sorting_template, true)->Arg(4);

// ------------------------------------
// ## Now that we know how fast algorithm works - lets scale it!
// ### And learn the rest of relevant functionality in the process
// ------------------------------------

template <typename execution_policy_t> static void supersort(bm::State &state, execution_policy_t &&policy) {

    auto count = static_cast<size_t>(state.range(0));
    std::vector<int32_t> array(count);
    std::iota(array.begin(), array.end(), 1);

    for (auto _ : state) {
        std::reverse(policy, array.begin(), array.end());
        std::sort(policy, array.begin(), array.end());
        bm::DoNotOptimize(array.size());
    }

    state.SetComplexityN(count);
    state.SetItemsProcessed(count * state.iterations());
    state.SetBytesProcessed(count * state.iterations() * sizeof(int32_t));

    // Feel free to report something else:
    // state.counters["tempreture_on_mars"] = bm::Counter(-95.4);
}

// Let's try running on 1M to 16M entries.
// This means input sizes between 4MB and 64MB respectively.
BENCHMARK_CAPTURE(supersort, seq, std::execution::seq)
    ->RangeMultiplier(8)
    ->Range(1l << 20, 1l << 32)
    ->MinTime(10)
    ->Complexity(benchmark::oNLogN);

BENCHMARK_CAPTURE(supersort, par_unseq, std::execution::par_unseq)
    ->RangeMultiplier(8)
    ->Range(1l << 20, 1l << 32)
    ->MinTime(10)
    ->Complexity(benchmark::oNLogN);

// Without `UseRealTime()`, CPU time is used by default.
// Difference example: when you sleep your process it is no longer accumulating CPU time.
// When you do syscall and switch contexts to create threads, you might face a problem here.
BENCHMARK_CAPTURE(supersort, par_unseq, std::execution::par_unseq)
    ->RangeMultiplier(8)
    ->Range(1l << 20, 1l << 32)
    ->MinTime(10)
    ->Complexity(benchmark::oNLogN)
    ->UseRealTime();

// ------------------------------------
// ## Practical Investigation Example
// ------------------------------------

BENCHMARK_MAIN();