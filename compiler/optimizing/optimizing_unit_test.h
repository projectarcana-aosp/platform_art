/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_COMPILER_OPTIMIZING_OPTIMIZING_UNIT_TEST_H_
#define ART_COMPILER_OPTIMIZING_OPTIMIZING_UNIT_TEST_H_

#include "base/scoped_arena_allocator.h"
#include "builder.h"
#include "common_compiler_test.h"
#include "dex/code_item_accessors-inl.h"
#include "dex/dex_file.h"
#include "dex/dex_instruction.h"
#include "driver/dex_compilation_unit.h"
#include "handle_scope-inl.h"
#include "mirror/class_loader.h"
#include "mirror/dex_cache.h"
#include "nodes.h"
#include "scoped_thread_state_change.h"
#include "ssa_builder.h"
#include "ssa_liveness_analysis.h"

#include "gtest/gtest.h"

namespace art {

#define NUM_INSTRUCTIONS(...)  \
  (sizeof((uint16_t[]) {__VA_ARGS__}) /sizeof(uint16_t))

#define N_REGISTERS_CODE_ITEM(NUM_REGS, ...)                            \
    { NUM_REGS, 0, 0, 0, 0, 0, NUM_INSTRUCTIONS(__VA_ARGS__), 0, __VA_ARGS__ }

#define ZERO_REGISTER_CODE_ITEM(...)   N_REGISTERS_CODE_ITEM(0, __VA_ARGS__)
#define ONE_REGISTER_CODE_ITEM(...)    N_REGISTERS_CODE_ITEM(1, __VA_ARGS__)
#define TWO_REGISTERS_CODE_ITEM(...)   N_REGISTERS_CODE_ITEM(2, __VA_ARGS__)
#define THREE_REGISTERS_CODE_ITEM(...) N_REGISTERS_CODE_ITEM(3, __VA_ARGS__)
#define FOUR_REGISTERS_CODE_ITEM(...)  N_REGISTERS_CODE_ITEM(4, __VA_ARGS__)
#define FIVE_REGISTERS_CODE_ITEM(...)  N_REGISTERS_CODE_ITEM(5, __VA_ARGS__)
#define SIX_REGISTERS_CODE_ITEM(...)   N_REGISTERS_CODE_ITEM(6, __VA_ARGS__)

LiveInterval* BuildInterval(const size_t ranges[][2],
                            size_t number_of_ranges,
                            ScopedArenaAllocator* allocator,
                            int reg = -1,
                            HInstruction* defined_by = nullptr) {
  LiveInterval* interval =
      LiveInterval::MakeInterval(allocator, DataType::Type::kInt32, defined_by);
  if (defined_by != nullptr) {
    defined_by->SetLiveInterval(interval);
  }
  for (size_t i = number_of_ranges; i > 0; --i) {
    interval->AddRange(ranges[i - 1][0], ranges[i - 1][1]);
  }
  interval->SetRegister(reg);
  return interval;
}

void RemoveSuspendChecks(HGraph* graph) {
  for (HBasicBlock* block : graph->GetBlocks()) {
    if (block != nullptr) {
      if (block->GetLoopInformation() != nullptr) {
        block->GetLoopInformation()->SetSuspendCheck(nullptr);
      }
      for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
        HInstruction* current = it.Current();
        if (current->IsSuspendCheck()) {
          current->GetBlock()->RemoveInstruction(current);
        }
      }
    }
  }
}

class ArenaPoolAndAllocator {
 public:
  ArenaPoolAndAllocator()
      : pool_(), allocator_(&pool_), arena_stack_(&pool_), scoped_allocator_(&arena_stack_) { }

  ArenaAllocator* GetAllocator() { return &allocator_; }
  ArenaStack* GetArenaStack() { return &arena_stack_; }
  ScopedArenaAllocator* GetScopedAllocator() { return &scoped_allocator_; }

 private:
  ArenaPool pool_;
  ArenaAllocator allocator_;
  ArenaStack arena_stack_;
  ScopedArenaAllocator scoped_allocator_;
};

inline HGraph* CreateGraph(ArenaPoolAndAllocator* pool_and_allocator) {
  return new (pool_and_allocator->GetAllocator()) HGraph(
      pool_and_allocator->GetAllocator(),
      pool_and_allocator->GetArenaStack(),
      *reinterpret_cast<DexFile*>(pool_and_allocator->GetAllocator()->Alloc(sizeof(DexFile))),
      /*method_idx*/-1,
      kRuntimeISA);
}

class OptimizingUnitTest : public CommonCompilerTest {
 protected:
  OptimizingUnitTest() : pool_and_allocator_(new ArenaPoolAndAllocator()) { }

  ArenaAllocator* GetAllocator() { return pool_and_allocator_->GetAllocator(); }
  ArenaStack* GetArenaStack() { return pool_and_allocator_->GetArenaStack(); }
  ScopedArenaAllocator* GetScopedAllocator() { return pool_and_allocator_->GetScopedAllocator(); }

  void ResetPoolAndAllocator() {
    pool_and_allocator_.reset(new ArenaPoolAndAllocator());
    handles_.reset();  // When getting rid of the old HGraph, we can also reset handles_.
  }

  HGraph* CreateGraph() {
    return art::CreateGraph(pool_and_allocator_.get());
  }

  // Create a control-flow graph from Dex instructions.
  HGraph* CreateCFG(const uint16_t* data, DataType::Type return_type = DataType::Type::kInt32) {
    const DexFile::CodeItem* code_item = reinterpret_cast<const DexFile::CodeItem*>(data);
    HGraph* graph = CreateGraph();

    {
      ScopedObjectAccess soa(Thread::Current());
      if (handles_ == nullptr) {
        handles_.reset(new VariableSizedHandleScope(soa.Self()));
      }
      const DexCompilationUnit* dex_compilation_unit =
          new (graph->GetAllocator()) DexCompilationUnit(
              handles_->NewHandle<mirror::ClassLoader>(nullptr),
              /* class_linker */ nullptr,
              graph->GetDexFile(),
              code_item,
              /* class_def_index */ DexFile::kDexNoIndex16,
              /* method_idx */ dex::kDexNoIndex,
              /* access_flags */ 0u,
              /* verified_method */ nullptr,
              handles_->NewHandle<mirror::DexCache>(nullptr));
      CodeItemDebugInfoAccessor accessor(graph->GetDexFile(), code_item);
      HGraphBuilder builder(graph, dex_compilation_unit, accessor, handles_.get(), return_type);
      bool graph_built = (builder.BuildGraph() == kAnalysisSuccess);
      return graph_built ? graph : nullptr;
    }
  }

 private:
  std::unique_ptr<ArenaPoolAndAllocator> pool_and_allocator_;
  std::unique_ptr<VariableSizedHandleScope> handles_;
};

// Naive string diff data type.
typedef std::list<std::pair<std::string, std::string>> diff_t;

// An alias for the empty string used to make it clear that a line is
// removed in a diff.
static const std::string removed = "";  // NOLINT [runtime/string] [4]

// Naive patch command: apply a diff to a string.
inline std::string Patch(const std::string& original, const diff_t& diff) {
  std::string result = original;
  for (const auto& p : diff) {
    std::string::size_type pos = result.find(p.first);
    DCHECK_NE(pos, std::string::npos)
        << "Could not find: \"" << p.first << "\" in \"" << result << "\"";
    result.replace(pos, p.first.size(), p.second);
  }
  return result;
}

// Returns if the instruction is removed from the graph.
inline bool IsRemoved(HInstruction* instruction) {
  return instruction->GetBlock() == nullptr;
}

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_OPTIMIZING_UNIT_TEST_H_
