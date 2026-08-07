//==--- DiagnosticTrapKinds.td ------------------------ -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// Trap Diagnostics
//
// These are diagnostics that are emitted into `TrapReason` objects using the
// `TrapReasonBuilder` class. These `TrapReason` objects are then encoded into
// debug info during codegen, rather than to the traditional diagnostic
// consumers like the terminal. Their primary purpose is to make debugging traps
// (e.g. `-fsanitize-trap=undefined`) easier by attaching a trap category and
// message to the trap instruction that tools like a debugger can show.
//
//===----------------------------------------------------------------------===//
let Component = "Trap" in {
let CategoryName = "Undefined Behavior Sanitizer" in {

def trap_ubsan_arith_overflow : Trap<
  "%select{unsigned|signed}0 integer "
  "%enum_select<UBSanArithKind>{"
    "%Add{addition}|"
    "%Sub{subtraction}|"
    "%Mul{multiplication}"
  "}1 overflow in %2">;

}

let CategoryName = "C++ Profiles" in {

// A runtime-checked profile rule's trap diagnostic is latent and sits in the
// rule's diagnostic group like a compile-time rule's (see ProfileRule in
// DiagnosticSemaKinds.td); its mapping decides whether the check is emitted.
def trap_profile_zero_divide : Trap<
  "integer division or remainder by zero under profile '%0'">,
  Latent, InGroup<ProfileTestArithZeroDivide>, ShowInSystemHeader;

// std::core_ub's zero_divide rule rides the same division and remainder check
// sites as the test::arith pilot with a trap diagnostic of its own, since a
// diagnostic belongs to one rule group.
def trap_profile_core_ub_zero_divide : Trap<
  "integer division or remainder by zero under profile '%0'">,
  Latent, InGroup<ProfileStdCoreUbZeroDivide>, ShowInSystemHeader;

def trap_profile_signed_overflow : Trap<
  "signed integer overflow under profile '%0'">,
  Latent, InGroup<ProfileStdCoreUbSignedOverflow>, ShowInSystemHeader;

}
}
