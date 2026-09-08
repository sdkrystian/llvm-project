// -fprofiles-enforce= is forwarded to -cc1 verbatim, repeatable with one or
// more names per occurrence; -cc1 implies -fprofiles from it. A winning
// -fno-profiles drops the enforcements, which the driver then reports unused.
// (CHECKs quote the flag with its value: the bare option name also appears in
// this file's own name in the -### output.)

// RUN: %clang -### -fprofiles-enforce=std::init -std=c++23 -c %s 2>&1 | FileCheck -check-prefix=ONE %s
// RUN: %clang -### -fprofiles-enforce=std::init,acme::hardened -fprofiles-enforce=test::arith -std=c++23 -c %s 2>&1 | FileCheck -check-prefix=MANY %s
// RUN: %clang -### -fno-profiles -fprofiles-enforce=std::init -fprofiles -std=c++23 -c %s 2>&1 | FileCheck -check-prefix=REENABLED %s
// RUN: %clang -### -fprofiles-enforce=std::init -fno-profiles -std=c++23 -c %s 2>&1 | FileCheck -check-prefix=OFF %s
// RUN: %clang -### -fno-profiles -fprofiles-enforce=std::init -std=c++23 -c %s 2>&1 | FileCheck -check-prefix=OFF %s

// ONE: "-fprofiles-enforce=std::init"
// MANY: "-fprofiles-enforce=std::init,acme::hardened" "-fprofiles-enforce=test::arith"
// REENABLED: "-fprofiles"
// REENABLED-SAME: "-fprofiles-enforce=std::init"
// OFF: warning: argument unused during compilation: '-fprofiles-enforce=std::init'
// OFF-NOT: "-fprofiles-enforce=std::init"
