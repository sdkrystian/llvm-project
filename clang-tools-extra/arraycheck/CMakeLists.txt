set(LLVM_LINK_COMPONENTS
  Support
  )

add_clang_executable(arraycheck
  ArrayCheck.cpp
  )

clang_target_link_libraries(arraycheck
  PRIVATE
  clangAST
  clangASTMatchers
  clangBasic
  clangFrontend
  clangTooling
  clangToolingRefactoring
  )
