// Module code is checked under the module's own enforcement wherever it is
// instantiated: an enforcing module's templates instantiated in an importer
// that neither enforces nor enables the profiles are diagnosed at every host
// (expression site, class-completion funnel, CFG rider), and a non-enforcing
// module's template instantiated in an enforcing importer is not.
// RUN: rm -rf %t && mkdir -p %t
// RUN: split-file %s %t
// RUN: %clang_cc1 -std=c++23 -fprofiles -fprofiles-test-profiles -emit-module-interface -o %t/m.pcm %t/m.cppm -verify
// RUN: %clang_cc1 -std=c++23 -fprofiles -fprofiles-test-profiles -fsyntax-only -fmodule-file=M=%t/m.pcm %t/use.cpp -verify
// RUN: %clang_cc1 -std=c++23 -fprofiles -fprofiles-test-profiles -emit-module-interface -o %t/n.pcm %t/n.cppm -verify
// RUN: %clang_cc1 -std=c++23 -fprofiles -fprofiles-test-profiles -fsyntax-only -fmodule-file=N=%t/n.pcm %t/use_n.cpp -verify

//--- m.cppm
// The templates are dependent, so nothing fires while the interface is built.
// expected-no-diagnostics
export module M [[profiles::enforce(test::type_cast, test::uninit_read, test::class_final)]];
export template <class T> int *mcast(T *p) { return reinterpret_cast<int *>(p); }
export template <class T> struct MClass { T m; };
export template <class T> T mread() {
  T x;
  return x;
}

//--- use.cpp
// The importer enforces and enables nothing; each instantiation fires in the
// module's dominion.
import M;
long g;
// expected-error@m.cppm:4 {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
int *use_cast() { return mcast(&g); } // expected-note {{in instantiation of function template specialization 'mcast<long>' requested here}}
// expected-error@m.cppm:5 {{test profile fired on completion of class 'MClass<int>' under profile 'test::class_final'}}
MClass<int> use_class; // expected-note {{in instantiation of template class 'MClass<int>' requested here}}
// expected-note@m.cppm:7 {{variable 'x' is declared here}}
// expected-error@m.cppm:8 {{variable 'x' is read before initialization under profile 'test::uninit_read'}}
int use_read() { return mread<int>(); } // expected-note {{in instantiation of function template specialization 'mread<int>' requested here}}

//--- n.cppm
// expected-no-diagnostics
export module N;
export template <class T> int *ncast(T *p) { return reinterpret_cast<int *>(p); }

//--- use_n.cpp
// The importer's enforcement does not reach the module's tokens; its own
// reinterpret_cast fires.
[[profiles::enforce(test::type_cast)]];
import N;
long h;
int *use_ncast() { return ncast(&h); }
int *use_local() { return reinterpret_cast<int *>(h); } // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
