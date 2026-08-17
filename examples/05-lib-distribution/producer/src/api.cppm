// An INTERFACE partition: `export module`, so it is part of the closure and
// its source travels with the package.
export module mathkit:api;

export namespace mk {
// Declarations only. The definitions live in the implementation units below
// and reach the consumer as the prebuilt archive.
int    add(int a, int b);
double scale(double v);
}
