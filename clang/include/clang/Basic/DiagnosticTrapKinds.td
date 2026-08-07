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

def trap_profile_zero_divide : Trap<
  "integer division or remainder by zero under profile '%0'">;

def trap_profile_signed_overflow : Trap<
  "signed integer overflow under profile '%0'">;

def trap_profile_invalid_shift : Trap<
  "shift out of bounds under profile '%0'">;

def trap_profile_misaligned_access : Trap<
  "misaligned pointer access under profile '%0'">;

def trap_profile_null_dereference : Trap<
  "null pointer dereference under profile '%0'">;

def trap_profile_out_of_bounds : Trap<
  "array index out of bounds under profile '%0'">;

def trap_profile_float_cast_overflow : Trap<
  "floating-point to integer conversion overflow under profile '%0'">;

def trap_profile_enum_out_of_range : Trap<
  "load of an out-of-range enumeration value under profile '%0'">;

}
}
