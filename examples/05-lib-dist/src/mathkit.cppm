// The published root. Everything a consumer can `import mathkit;` reaches from
// here — and `mcpp pack` publishes exactly that closure, nothing else.
export module mathkit;
export import :api;
