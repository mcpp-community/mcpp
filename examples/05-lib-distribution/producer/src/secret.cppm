// An IMPLEMENTATION partition: `module` with no `export`.
//
// It produces a BMI and a `.m.o` exactly like the interface units do — which
// is why "publish every module unit" would leak it. It is NOT reachable from
// the interface's purview, so `mcpp pack` withholds its source and keeps its
// object in the archive.
module mathkit:secret;

namespace mk {
// Pretend this is the part you are not shipping as source.
double house_factor() { return 2.5; }
}
