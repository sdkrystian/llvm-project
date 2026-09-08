// A -fprofiles-enforce= enforcement is local to the translation unit it is
// given to: it covers the whole unit, global module fragment included, but a
// module interface or header unit built with it advertises nothing for
// [[profiles::require]], and an implementation unit does not inherit it.

// RUN: rm -rf %t
// RUN: mkdir -p %t
// RUN: split-file %s %t

// RUN: %clang_cc1 -std=c++20 -fprofiles-test-profiles -fprofiles-enforce=test::type_cast -fsyntax-only %t/gmf.cppm -verify
// RUN: %clang_cc1 -std=c++20 -fprofiles-test-profiles -fprofiles-enforce=test::type_cast -emit-module-interface %t/iface.cppm -o %t/iface.pcm -verify
// RUN: %clang_cc1 -std=c++20 -fprofiles -fsyntax-only %t/require.cpp -fmodule-file=CmdMod=%t/iface.pcm -verify
// RUN: %clang_cc1 -std=c++20 -fprofiles -fprofiles-test-profiles -fsyntax-only %t/impl.cpp -fmodule-file=CmdMod=%t/iface.pcm -verify
// RUN: %clang_cc1 -std=c++20 -fprofiles-test-profiles -fprofiles-enforce=test::type_cast -fsyntax-only %t/impl.cpp -fmodule-file=CmdMod=%t/iface.pcm -verify=enforced
// RUN: %clang_cc1 -std=c++20 -fprofiles-test-profiles -fprofiles-enforce=test::type_cast -emit-header-unit -xc++-user-header %t/hu.h -o %t/hu.pcm
// RUN: %clang_cc1 -std=c++20 -fprofiles -Wno-experimental-header-units -fsyntax-only %t/require_hu.cpp -fmodule-file=%t/hu.pcm -verify

//--- gmf.cppm
// The global module fragment lies inside the option's dominion, unlike the
// dominion of an enforcement written on the module-declaration.
module;
int *gmf = reinterpret_cast<int*>(0); // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}
export module GmfMod;
int *purview = reinterpret_cast<int*>(0); // expected-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}

//--- iface.cppm
// expected-no-diagnostics
export module CmdMod;
export int f();

//--- require.cpp
// The interface was built with -fprofiles-enforce=test::type_cast, which
// advertises nothing.
import CmdMod [[profiles::require(test::type_cast)]]; // expected-error {{required profile 'test::type_cast' is not enforced by imported module}}

//--- impl.cpp
// An implementation unit inherits the interface's advertised profiles only,
// so the interface's command-line enforcement does not reach it; the option
// enforces here when given to this unit.
// expected-no-diagnostics
module CmdMod;
int *impl = reinterpret_cast<int*>(0); // enforced-error {{'reinterpret_cast' is unsafe under profile 'test::type_cast'}}

//--- hu.h
int hu_decl();

//--- require_hu.cpp
// The header unit was built with -fprofiles-enforce=test::type_cast, which
// advertises nothing.
import "hu.h" [[profiles::require(test::type_cast)]]; // expected-error {{required profile 'test::type_cast' is not enforced by imported module}}
