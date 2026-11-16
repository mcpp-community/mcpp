import std;
import mcpplibs.mtest;
import canary;
using namespace mcpplibs::mtest;

UTest sum{"Canary/ThreadsSum", [] { expect(eq(canary::parallel_sum(100), 5050LL)); }};
UTest exc{"Canary/ExceptionsCarry", [] { expect(eq(canary::thrown_and_caught(), std::string("carried"))); }};
