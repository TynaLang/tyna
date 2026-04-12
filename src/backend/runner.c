#include <stdio.h>
#include <stdlib.h>

#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/LLJIT.h>
#include <llvm-c/Orc.h>
#include <llvm-c/OrcEE.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

#include "codegen_private.h"
#include "tyl/codegen.h"
#include "tyl/runner.h"

void Runner_verify(Codegen *cg) {
  char *error = NULL;

  if (LLVMVerifyModule(cg->module, LLVMReturnStatusAction, &error)) {
    fprintf(stderr, "LLVM verification failed:\n%s\n", error);
    fprintf(stderr, "=== LLVM module dump ===\n");
    LLVMDumpModule(cg->module);
    LLVMDisposeMessage(error);
    exit(1);
  }
}

void Runner_emit_ir(Codegen *cg, const char *filename) {
  if (LLVMPrintModuleToFile(cg->module, filename, NULL) != 0) {
    fprintf(stderr, "Failed to write IR to file\n");
    exit(1);
  }
}

void Runner_emit_object(Codegen *cg, const char *filename) {
  LLVMInitializeNativeTarget();
  LLVMInitializeNativeAsmPrinter();
  LLVMInitializeNativeAsmParser();

  char *error = NULL;
  LLVMTargetRef target;

  if (LLVMGetTargetFromTriple(LLVMGetDefaultTargetTriple(), &target, &error)) {
    fprintf(stderr, "Failed to get target: %s\n", error);
    LLVMDisposeMessage(error);
    exit(1);
  }

  LLVMTargetMachineRef machine = LLVMCreateTargetMachine(
      target, LLVMGetDefaultTargetTriple(), "generic", "",
      LLVMCodeGenLevelDefault, LLVMRelocPIC, LLVMCodeModelDefault);

  if (LLVMTargetMachineEmitToFile(machine, cg->module, (char *)filename,
                                  LLVMObjectFile, &error)) {
    fprintf(stderr, "Failed to emit object file: %s\n", error);
    LLVMDisposeMessage(error);
    exit(1);
  }

  LLVMDisposeTargetMachine(machine);
}

void Runner_link_executable(const char *obj, const char *out) {
  char cmd[1024];

  snprintf(cmd, sizeof(cmd),
           "clang -Iinclude %s src/shared/alloc.c src/shared/utils.c "
           "src/shared/runtime.c -o %s -lm",
           obj, out);

  int res = system(cmd);
  if (res != 0) {
    fprintf(stderr, "Linking failed\n");
    exit(1);
  }
}

void Runner_jit(Codegen *cg) {
  LLVMInitializeNativeTarget();
  LLVMInitializeNativeAsmPrinter();
  LLVMInitializeNativeAsmParser();

  LLVMOrcLLJITRef lljit;
  LLVMOrcLLJITBuilderRef builder = LLVMOrcCreateLLJITBuilder();

  LLVMErrorRef err = LLVMOrcCreateLLJIT(&lljit, builder);
  if (err) {
    char *msg = LLVMGetErrorMessage(err);
    fprintf(stderr, "Failed to create ORC LLJIT: %s\n", msg);
    LLVMDisposeErrorMessage(msg);
    exit(1);
  }

  LLVMOrcJITDylibRef main_jd = LLVMOrcLLJITGetMainJITDylib(lljit);

  LLVMOrcDefinitionGeneratorRef dg;
  char prefix = LLVMOrcLLJITGetGlobalPrefix(lljit);
  err = LLVMOrcCreateDynamicLibrarySearchGeneratorForProcess(&dg, prefix, NULL,
                                                             NULL);
  if (!err) {
    LLVMOrcJITDylibAddGenerator(main_jd, dg);
  }

  LLVMOrcThreadSafeContextRef ts_ctx = LLVMOrcCreateNewThreadSafeContext();
  LLVMOrcThreadSafeModuleRef ts_mod =
      LLVMOrcCreateNewThreadSafeModule(cg->module, ts_ctx);

  err = LLVMOrcLLJITAddLLVMIRModule(lljit, main_jd, ts_mod);
  if (err) {
    char *msg = LLVMGetErrorMessage(err);
    fprintf(stderr, "Failed to add module to JIT: %s\n", msg);
    LLVMDisposeErrorMessage(msg);
    exit(1);
  }

  LLVMOrcExecutorAddress entry_addr;
  err = LLVMOrcLLJITLookup(lljit, &entry_addr, "main");
  if (err) {
    char *msg = LLVMGetErrorMessage(err);
    fprintf(stderr, "Failed to find entry point 'main': %s\n", msg);
    LLVMDisposeErrorMessage(msg);
    exit(1);
  }

  int (*entry_func)() = (int (*)())(uintptr_t)entry_addr;
  int exit_code = entry_func();

  LLVMOrcDisposeLLJIT(lljit);
  LLVMOrcDisposeThreadSafeContext(ts_ctx);
}