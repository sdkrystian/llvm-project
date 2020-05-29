// Copyright(c) 2020 Krystian Stasiowski

#define _SILENCE_ALL_CXX17_DEPRECATION_WARNINGS

#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/AST/Type.h"
#include "clang/Basic/TargetInfo.h"
#include "llvm/Support/Host.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/CommandLine.h"

#include <iostream>
#include <filesystem>
#include <future>
#include <utility>
#include <memory>
#include <vector>
#include <list>
#include <utility>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <atomic>

namespace arraycheck
{
  using namespace clang;
  using namespace clang::ast_matchers;

  // i know no other way to do this
  static std::atomic<bool> terminated = false;

  namespace array_matchers
  {
    auto is_decayed_array_type = 
      hasType(decayedType(hasDecayedType(pointerType(unless(pointee(hasCanonicalType(functionType())))))));
    auto param_matcher = 
      functionDecl(hasAnyParameter(is_decayed_array_type)).bind("func_array_param");
    auto nttp_matcher = 
      nonTypeTemplateParmDecl(is_decayed_array_type).bind("nttp_array_param");
    auto catch_matcher = 
      varDecl(hasParent(cxxCatchStmt()), has(arrayType())).bind("catch_array_param");
  } // array_matchers

  struct match_result
  {
    std::string file;
    std::size_t functions = 0;
    std::size_t nttps = 0;
    std::size_t catches = 0;
    std::size_t main = 0;
  };

  class match_callback : public MatchFinder::MatchCallback
  {
  public:
    match_callback() = default;

    match_callback& operator=(std::string&& file)
    {
      // we only use a single callback per worker,
      // so we steal the file string 
      result_.file = std::move(file);
      // reset the result counts
      result_.functions = 0; 
      result_.nttps = 0;
      result_.catches = 0;
      result_.main = 0;
      return *this;
    }

    std::string& current_file()
    {
      return result_.file;
    }

    match_result get_result()
    {
      return result_;
    }

  private:
    virtual void run(const MatchFinder::MatchResult& result)
    {
      if (const auto decl = result.Nodes.getNodeAs<FunctionDecl>("func_array_param"))
      {
        ++result_.functions;
        if (decl->isMain())
          ++result_.main;
      }
      else if (const auto decl = result.Nodes.getNodeAs<NonTypeTemplateParmDecl>("nttp_array_param"))
        ++result_.nttps;
      else if (const auto decl = result.Nodes.getNodeAs<VarDecl>("catch_array_param"))
        ++result_.catches;
    }

    match_result result_;
  };


  class worker_pool
  {
  public:
    worker_pool(std::size_t num_workers)
      : workers_(num_workers)
    {
      free_workers_.reserve(num_workers);
      for (auto& wrk : workers_)
        free_workers_.push_back(&wrk);
    }

    bool run_worker(std::string&& file)
    {
      worker& free = get_free_worker();
      bool result = free.get_result();
      free.execute(std::move(file), 
        [this, &free](match_result&& result, bool sucess)
        {
          // this means the thread was
          // orphaned, so dont output
          if (terminated)
            return;
          std::unique_lock<std::mutex> complete_lock(complete_mut_);
          // endl is used here because we want to flush
          if (sucess)
            if (result.functions || result.nttps || result.catches)
              std::cout << result.file << '|' << result.functions << '|' <<
              result.main << '|' << result.nttps << '|' << result.catches << std::endl;
            else
              std::cout << result.file << "|none" << std::endl;
          else
            std::cerr << result.file << "|error\n";
          set_free_worker(free);
        });
      return result;
    }

    bool keep_alive_until_complete(
      std::size_t timeout)
    {
      bool result = true;
      for (worker& wrk : workers_)
      {
        bool worker_result = wrk.rush_result(timeout);
        if (!worker_result)
        {
          std::unique_lock<std::mutex> complete_lock(complete_mut_);
          std::cerr << "Compiler errored or timed out\n";
        }
        result &= worker_result;
      }
      if (!result)
        terminated = true;
      return result;
    }

  private:
    class worker
    {
    public:
      worker()
      {
        // add all matchers to the match finder
        finder_.addMatcher(array_matchers::param_matcher, &callback_);
        finder_.addMatcher(array_matchers::nttp_matcher, &callback_);
        finder_.addMatcher(array_matchers::catch_matcher, &callback_);
        // create a frontend action and store it
        action_ = clang::tooling::newFrontendActionFactory(&finder_)->create();
      }

      worker(const worker&) = delete;
      worker(worker&&) noexcept = default;
      worker& operator=(const worker&) = delete;
      worker& operator=(worker&&) noexcept = default;

      template<typename Handler>
      void execute(std::string&& file, Handler handler)
      {
        // create a new instance to avoid bugs resulting
        // from any previous runs. safety overspeed here,
        // it rather have it run a little slower over having
        // to restart the tool often
        instance_.~CompilerInstance();
        new (&instance_) CompilerInstance;
        setup_intance();
        // move the file string to the call back, and obtain a reference
        // to it so we can pass it to the frontend. DONT do this async,
        // as moving the file could introduce a data race.
        auto& moved_file = (callback_ = std::move(file)).current_file();
        auto& inputs = instance_.getFrontendOpts().Inputs;
        // clear the input list if any inputs are present
        if (!inputs.empty())
          inputs.clear();
        inputs.emplace_back(moved_file, Language::CXX, false);
        result_ = std::async(std::launch::async,
          [&, handler = std::move(handler)]()
        {
          // execute the action
          bool result = false;
          try
          {
            result = instance_.ExecuteAction(*action_);
          }
          catch (...)
          {
            // reset the frontend action.
            action_ = clang::tooling::newFrontendActionFactory(&finder_)->create();
            // this just is a quick way to reset the counters
            callback_ = std::move(callback_.get_result().file);
          }
          handler(std::move(callback_.get_result()), result);
          return result;
        });
      }

      bool get_result()
      {
        if (result_.valid())
          return result_.get();
        return true;
      }

      bool rush_result(std::size_t timeout)
      {
        // we want to close the program
        // the worker can either finish
        // in the specified duration or 
        // or die with the main thread
        if (result_.valid())
        {
          if (result_.wait_for(
            std::chrono::seconds(timeout)) ==
              std::future_status::timeout)
            return false;
          else
            return result_.get();
        }
        return true;
      }

    private:
      std::shared_ptr<CompilerInvocation> create_invocation()
      {
        auto invocation = std::make_shared<CompilerInvocation>();
        auto& frontend_opt = invocation->getFrontendOpts();
        // only parse syntax
        frontend_opt.ProgramAction = frontend::ParseSyntaxOnly;
        // skip function bodies since we generally dont care about
        // block-scope function declarations, as they only are useful
        // for redeclaring functions in the same or another TU. Local
        // classes are rare, so we dont care about those either
        frontend_opt.SkipFunctionBodies = true;
        auto& tgt_opt = invocation->getTargetOpts();
        tgt_opt.Triple = llvm::sys::getDefaultTargetTriple();
        auto& pp_opt = invocation->getPreprocessorOpts();
        // this causes include directives to not perform
        // copying from the included file
        pp_opt.SingleFileParseMode = true;
        // use the default options for C++20
        CompilerInvocation::setLangDefaults(*(invocation->getLangOpts()), Language::CXX,
          llvm::Triple(tgt_opt.Triple), pp_opt, LangStandard::lang_cxx17);
        return invocation;
      }

      void setup_intance()
      {
        instance_.createDiagnostics(new IgnoringDiagConsumer());
        auto& diag = instance_.getDiagnostics();
        // allows infinite errors, treats fatal errors
        // (i.e. include not found) as normal errors,
        // ignores all warning, and does not
        // report errors to the user
        diag.setErrorLimit(0);
        diag.setFatalsAsError(true);
        diag.setIgnoreAllWarnings(true);
        // debug pragmas can instruct the preprocessor to
        // crash, dump the ast, or a number of other bad
        // things that we dont want happening, so we disable them.
        // setup the instance to use our invocation
        instance_.setInvocation(create_invocation());
      }

      CompilerInstance instance_;
      MatchFinder finder_;
      match_callback callback_;
      std::unique_ptr<FrontendAction> action_;
      std::future<bool> result_;
    };

    worker& get_free_worker()
    {
      std::unique_lock<std::mutex> free_lock(free_mut_);
      if (free_workers_.empty())
        free_cv_.wait(free_lock, [this] { return !free_workers_.empty(); });
      worker& free = *free_workers_.back();
      free_workers_.pop_back();
      return free;
    }

    void set_free_worker(worker& free)
    {
      std::unique_lock<std::mutex> free_lock(free_mut_);
      free_workers_.push_back(&free);
      free_cv_.notify_one();
    }

    std::list<worker> workers_;
    std::vector<worker*> free_workers_;
    std::mutex free_mut_;
    std::mutex complete_mut_;
    std::condition_variable free_cv_;
  };

  void process_files(
    std::vector<std::string>&& files,
    std::size_t num_workers,
    std::size_t timeout)
  {
    auto files_count = files.size();
    // we should not have more workers than
    // files to process
    if (num_workers > files_count)
      num_workers = files_count;
    worker_pool pool(num_workers);
    for (auto& file : files)
      pool.run_worker(std::move(file));
    // this will probably not blow up
    if (!pool.keep_alive_until_complete(timeout))
      std::exit(1);
  }
} // array check

int main(int argc, const char** argv)
{
  auto workers = 8;
  auto timeout = 30;
  if (argc == 4)
  {
    if ((timeout = std::atoll(argv[3])) <= 0)
    {
      std::cerr << "The timeout duration must be greater than 0\n";
      return 1;
    }
  }
  if (argc >= 3 && argc <= 4)
  {
    if ((workers = std::atoll(argv[2])) <= 0)
    {
      std::cerr << "The number of workers must be greater than 0\n";
      return 1;
    }
  }
  else if (argc != 2)
  {
    std::cerr << "Usage: arraycheck <path> <workers>\n";
    return 1;
  }
  namespace fs = std::filesystem;
  std::vector<std::string> files;
  for (const auto& file : fs::recursive_directory_iterator(argv[1]))
    if (!fs::is_directory(file))
      files.push_back(fs::canonical(file.path()).generic_string());
  if (files.empty())
  {
    std::cerr << "No files were found\n";
    return 1;
  }
  arraycheck::process_files(std::move(files), workers, timeout);
  return 0;
}