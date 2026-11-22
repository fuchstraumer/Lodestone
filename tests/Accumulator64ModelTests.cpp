#include "TestHarness.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

// A model of the WGSL implementation of IAtomicAccumulator64 (BoundAccumulator64Words in builtins/LodestoneShims.slang), which
// runs on the CPU because no GPU is available to the tests. Each Add is split into the steps that the emitted WGSL runs: an
// atomic add on the low word, the carry from the old low word, and an atomic add on the high word. A step is atomic and the
// steps of different threads interleave in a random order. The test proves the arithmetic of the algorithm. It does not prove
// that the emitted WGSL implements it: ShimPlacementTest checks the text of that code, and a reader compares it with this file.

namespace
{

struct Words
{
    uint32_t Low{ 0u };
    uint32_t High{ 0u };
};

/** The two words of a signed 64-bit value, as a host reads them from an int64_t. */
Words WordsOf(int64_t value) noexcept;

/** Wide64.Negated of the shader, written with the same operations on 32-bit words. */
Words Negated(Words value) noexcept;

/** Wide64.FromInt of the shader. */
Words FromInt(int32_t value) noexcept;

/** Wide64.ToFloat of the shader. */
float ToFloat(Words value) noexcept;

/** The value that a host reads from the 8 bytes of one element. */
int64_t ValueOf(Words words) noexcept;

enum class Variant : uint8_t
{
    /** The algorithm of BoundAccumulator64Words. */
    Correct,
    /** Control: the carry is dropped. */
    NoCarry,
    /** Control: the low word is read and written back in two steps, which is not an atomic add. */
    SplitLowAdd,
};

/** One thread: the 32-bit words it adds, in order, and where it is in the current Add. */
struct ModelThread
{
    std::vector<Words> Terms;
    /** The value of each term before an Add or a Sub made words of it, to compute the expected sum independently. */
    std::vector<int64_t> Values;
    size_t NextTerm{ 0u };
    uint32_t Step{ 0u };
    uint32_t OldLow{ 0u };
    uint32_t Carry{ 0u };
};

bool Finished(const ModelThread& thread) noexcept;

/** Runs one step of one thread against the shared element. */
void RunStep(ModelThread& thread, Words& element, Variant variant) noexcept;

/** Runs every thread to the end, choosing the next thread at random for each step, and returns the element. */
Words RunInterleaved(std::vector<ModelThread> threads, std::mt19937_64& random, Variant variant);

/** Values near every boundary of a 32-bit word, a sign, and the 64-bit range, then random values. */
int64_t PickValue(std::mt19937_64& random);

/** The sum of every value of every thread, wrapped modulo 2^64 and negated for a Sub. */
uint64_t WrappedSum(const std::vector<ModelThread>& threads, bool use_sub) noexcept;

std::vector<ModelThread> MakeThreads(std::mt19937_64& random, bool use_sub);

} // namespace

int main()
{
    lodestone::tests::TestRunner runner{ "Accumulator64Model" };

    runner.BeginSection("the word helpers equal the 64-bit value");
    constexpr std::array<int64_t, 11u> k_Edges{ 0,
                                                1,
                                                -1,
                                                0x7FFFFFFF,
                                                0x80000000LL,
                                                0xFFFFFFFFLL,
                                                0x100000000LL,
                                                -0x100000000LL,
                                                -0x80000000LL,
                                                std::numeric_limits<int64_t>::max(),
                                                std::numeric_limits<int64_t>::min() };
    bool negatedAgrees = true;
    for (const int64_t edge : k_Edges)
    {
        const uint64_t negatedBits = uint64_t{ 0u } - std::bit_cast<uint64_t>(edge);
        const Words negated = Negated(WordsOf(edge));
        negatedAgrees = negatedAgrees && ValueOf(negated) == std::bit_cast<int64_t>(negatedBits) &&
                        ValueOf(WordsOf(edge)) == edge;
    }
    runner.Check(negatedAgrees, "Negated is two's complement negation at every edge, including INT64_MIN and a zero low word");

    bool fromIntAgrees = true;
    for (const int32_t value :
         { 0, 1, -1, 123456789, -123456789, std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::min() })
    {
        fromIntAgrees = fromIntAgrees && ValueOf(FromInt(value)) == static_cast<int64_t>(value);
    }
    runner.Check(fromIntAgrees, "FromInt sign extends");

    runner.BeginSection("a single thread, one Add at a time, gives the wrapped sum");
    std::mt19937_64 random{ 20261118u };
    bool serialAgrees = true;
    for (const int64_t first : k_Edges)
    {
        for (const int64_t second : k_Edges)
        {
            ModelThread thread;
            thread.Terms = { WordsOf(first), WordsOf(second) };
            Words element;
            while (!Finished(thread))
            {
                RunStep(thread, element, Variant::Correct);
            }

            const uint64_t expected = std::bit_cast<uint64_t>(first) + std::bit_cast<uint64_t>(second);
            serialAgrees = serialAgrees && std::bit_cast<uint64_t>(ValueOf(element)) == expected;
        }
    }
    runner.Check(serialAgrees, "every pair of edge values, including a carry from the low word and an overflow of the high word");

    runner.BeginSection("many threads in random interleavings give the wrapped sum");
    constexpr int32_t k_Runs = 3000;
    int32_t addFailures = 0;
    int32_t subFailures = 0;
    for (int32_t run = 0; run < k_Runs; ++run)
    {
        const bool useSub = (run & 1) != 0;
        const std::vector<ModelThread> threads = MakeThreads(random, useSub);
        const Words element = RunInterleaved(threads, random, Variant::Correct);
        const uint64_t expected = WrappedSum(threads, useSub);
        if (std::bit_cast<uint64_t>(ValueOf(element)) != expected)
        {
            ++(useSub ? subFailures : addFailures);
        }
    }
    runner.CheckEqual(addFailures, 0, "Add: every interleaving gives the wrapped sum");
    runner.CheckEqual(subFailures, 0, "Sub, which adds the negated words: every interleaving gives the wrapped sum");

    runner.BeginSection("the model can fail: two broken algorithms are caught");
    int32_t noCarryFailures = 0;
    int32_t splitFailures = 0;
    for (int32_t run = 0; run < k_Runs; ++run)
    {
        const std::vector<ModelThread> threads = MakeThreads(random, false);
        const uint64_t expected = WrappedSum(threads, false);
        const Words withoutCarry = RunInterleaved(threads, random, Variant::NoCarry);
        const Words withSplitAdd = RunInterleaved(threads, random, Variant::SplitLowAdd);

        noCarryFailures += std::bit_cast<uint64_t>(ValueOf(withoutCarry)) != expected ? 1 : 0;
        splitFailures += std::bit_cast<uint64_t>(ValueOf(withSplitAdd)) != expected ? 1 : 0;
    }
    runner.Check(noCarryFailures > k_Runs / 4, "an algorithm with no carry fails in many runs");
    runner.Check(splitFailures > 0, "a low word that is not added atomically fails in some runs");

    runner.BeginSection("ToFloat of the shader rounds the signed value, and a small negative value does not cancel to zero");
    bool floatAgrees = true;
    for (const int64_t edge : k_Edges)
    {
        const float computed = ToFloat(WordsOf(edge));
        const double exact = static_cast<double>(edge);
        const double error = std::abs(static_cast<double>(computed) - exact);
        floatAgrees = floatAgrees && error <= std::max(1.0, std::abs(exact)) * 1.2e-7 * 2.0;
    }
    runner.Check(floatAgrees, "ToFloat is within two float roundings of the value at every edge");

    return runner.Report();
}

namespace
{

Words WordsOf(int64_t value) noexcept
{
    const uint64_t bits = std::bit_cast<uint64_t>(value);

    return Words{ .Low = static_cast<uint32_t>(bits & 0xFFFFFFFFu), .High = static_cast<uint32_t>(bits >> 32u) };
}

Words Negated(Words value) noexcept
{
    return Words{ .Low = ~value.Low + 1u, .High = ~value.High + (value.Low == 0u ? 1u : 0u) };
}

Words FromInt(int32_t value) noexcept
{
    return Words{ .Low = static_cast<uint32_t>(value), .High = value < 0 ? 0xFFFFFFFFu : 0u };
}

float ToFloat(Words value) noexcept
{
    const bool negative = static_cast<int32_t>(value.High) < 0;
    const Words magnitude = negative ? Negated(value) : value;
    const float result = static_cast<float>(magnitude.High) * 4294967296.0f + static_cast<float>(magnitude.Low);

    return negative ? -result : result;
}

int64_t ValueOf(Words words) noexcept
{
    return std::bit_cast<int64_t>((static_cast<uint64_t>(words.High) << 32u) | words.Low);
}

bool Finished(const ModelThread& thread) noexcept
{
    return thread.NextTerm >= thread.Terms.size();
}

void RunStep(ModelThread& thread, Words& element, Variant variant) noexcept
{
    const Words term = thread.Terms[thread.NextTerm];

    if (variant == Variant::SplitLowAdd)
    {
        // step 0 reads the low word, step 1 writes it back with the sum, step 2 adds the high word
        if (thread.Step == 0u)
        {
            thread.OldLow = element.Low;
            thread.Step = 1u;
        }
        else if (thread.Step == 1u)
        {
            element.Low = thread.OldLow + term.Low;
            thread.Carry = element.Low < thread.OldLow ? 1u : 0u;
            thread.Step = 2u;
        }
        else
        {
            element.High += term.High + thread.Carry;
            thread.Step = 0u;
            ++thread.NextTerm;
        }

        return;
    }

    if (thread.Step == 0u)
    {
        // atomicAdd(low, term.low) returns the old low word
        thread.OldLow = element.Low;
        element.Low += term.Low;

        const uint32_t carry = (thread.OldLow + term.Low) < thread.OldLow ? 1u : 0u;
        thread.Carry = variant == Variant::NoCarry ? 0u : carry;
        thread.Step = 1u;
    }
    else
    {
        // atomicAdd(high, term.high + carry)
        element.High += term.High + thread.Carry;
        thread.Step = 0u;
        ++thread.NextTerm;
    }
}

Words RunInterleaved(std::vector<ModelThread> threads, std::mt19937_64& random, Variant variant)
{
    Words element;
    std::vector<size_t> live;
    for (size_t index = 0u; index < threads.size(); ++index)
    {
        live.push_back(index);
    }

    while (!live.empty())
    {
        const size_t pick = std::uniform_int_distribution<size_t>{ 0u, live.size() - 1u }(random);
        ModelThread& thread = threads[live[pick]];
        RunStep(thread, element, variant);

        if (Finished(thread))
        {
            live[pick] = live.back();
            live.pop_back();
        }
    }

    return element;
}

int64_t PickValue(std::mt19937_64& random)
{
    constexpr std::array<int64_t, 14u> k_Boundaries{ 0,
                                                     1,
                                                     -1,
                                                     0x7FFFFFFF,
                                                     0x80000000LL,
                                                     0xFFFFFFFFLL,
                                                     0x100000000LL,
                                                     -0x100000000LL,
                                                     0x100000001LL,
                                                     -0xFFFFFFFFLL,
                                                     0xFFFFFFFFFFFFLL,
                                                     -0x7FFFFFFFFFFFFFFFLL,
                                                     std::numeric_limits<int64_t>::max(),
                                                     std::numeric_limits<int64_t>::min() };
    const uint32_t choice = static_cast<uint32_t>(random() % 4u);
    if (choice == 0u)
    {
        return k_Boundaries[random() % k_Boundaries.size()];
    }

    if (choice == 1u)
    {
        // a small signed value, which is the fixed-point case and crosses zero often
        return static_cast<int64_t>(random() % 2001u) - 1000;
    }

    if (choice == 2u)
    {
        // a value with a low word that is close to a carry
        const int64_t high = static_cast<int64_t>(random() % 5u) - 2;
        const int64_t low = static_cast<int64_t>(0xFFFFFFFFu - (random() % 4u));
        return high * 0x100000000LL + low;
    }

    return std::bit_cast<int64_t>(random());
}

uint64_t WrappedSum(const std::vector<ModelThread>& threads, bool use_sub) noexcept
{
    uint64_t sum = 0u;
    for (const ModelThread& thread : threads)
    {
        for (const int64_t value : thread.Values)
        {
            sum += std::bit_cast<uint64_t>(value);
        }
    }

    return use_sub ? uint64_t{ 0u } - sum : sum;
}

std::vector<ModelThread> MakeThreads(std::mt19937_64& random, bool use_sub)
{
    const size_t threadCount = 2u + random() % 63u;
    std::vector<ModelThread> threads(threadCount);
    for (ModelThread& thread : threads)
    {
        const size_t termCount = 1u + random() % 8u;
        for (size_t term = 0u; term < termCount; ++term)
        {
            const int64_t value = PickValue(random);
            // a Sub of x adds the negated words of x
            thread.Values.push_back(value);
            thread.Terms.push_back(use_sub ? Negated(WordsOf(value)) : WordsOf(value));
        }
    }

    return threads;
}

} // namespace
