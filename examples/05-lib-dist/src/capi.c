/* The header interface's implementation. A consumer that only #includes
   mathkit_c.h never compiles a single module unit. */
int mathkit_add(int a, int b) { return a + b; }
