// { dg-additional-options {-fmodules-ts -fdump-lang-module-graph-blocks} }

export module TPL;
// { dg-module-cmi TPL }

export template <typename T> int foo (T x) 
{
  return int (x);
}

// Body is emitted in module-unit itself
template <> int foo<int> (int y)
{
  return 0;
}

// { dg-final { scan-lang-dump {Dependencies of specialization function_decl:'::foo<int>'} module } }
// { dg-final { scan-lang-dump-not {Depending definition function_decl:'::foo<int>'} module } }
// { dg-final { scan-lang-dump {Cluster members:\n  \[0\]=specialization declaration '::foo<int>'} module } }
// { dg-final { scan-lang-dump {Specialization '::foo<int>' entity:[0-9]* keyed to TPL\[0\] '::template foo'} module } }

// { dg-final { scan-assembler {_Z3fooIiEiT_:} } }
