/* { dg-do run } */
/* { dg-options "-fdump-tree-alias" } */
/* { dg-skip-if "" { *-*-* } { "-O0" } { "" } } */

volatile int i;
int ** __attribute__((noinline,pure)) foo(int **p) { i; return p; }
int bar(void)
{
  int i = 0, j = 1;
  int *p, **q;
  p = &i;
  q = foo(&p);
  *q = &j;
  return *p;
}
extern void abort (void);
int main()
{
  if (bar() != 1)
    abort ();
  return 0;
}

/* { dg-final { scan-tree-dump "p.._., name memory tag: NMT..., is dereferenced, points-to vars: { i j }" "alias" } } */
/* { dg-final { cleanup-tree-dump "alias" } } */
