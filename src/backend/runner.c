#include <stdio.h>
#include <stdlib.h>

#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

#include "codegen.h"

void Runner_verify(Codegen *cg) {
  char *error = NULL;

  if (LLVMVerifyModule(cg->module, LLVMAbortProcessAction, &error)) {
    fprintf(stderr, "LLVM verification failed:\n%s\n", error);
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
  char cmd[512];

  // links against libc automatically (printf)
  snprintf(cmd, sizeof(cmd), "clang %s -o %s", obj, out);

  int res = system(cmd);
  if (res != 0) {
    fprintf(stderr, "Linking failed\n");
    exit(1);
  }
}

void Runner_jit(Codegen *cg) {
  LLVMInitializeNativeTarget();
  LLVMInitializeNativeAsmPrinter();

  LLVMLinkInMCJIT();

  LLVMExecutionEngineRef engine;
  char *error = NULL;

  if (LLVMCreateExecutionEngineForModule(&engine, cg->module, &error) != 0) {
    fprintf(stderr, "Failed to create execution engine: %s\n", error);
    LLVMDisposeMessage(error);
    exit(1);
  }

  CGFunction *printf_func = NULL;
  for (size_t i = 0; i < cg->system_functions.len; i++) {
    CGFunction *f = cg->system_functions.items[i];
    if (sv_eq(f->name, sv_from_parts("print", 5))) {
      printf_func = f;
      break;
    }
  }
  if (printf_func) {
    LLVMAddGlobalMapping(engine, printf_func->value, (void *)&printf);
  } else {
    fprintf(stderr, "Warning: printf function not found for JIT mapping\n");
  }

  // run main()
  LLVMValueRef main_func = LLVMGetNamedFunction(cg->module, "main");

  if (!main_func) {
    fprintf(stderr, "No main function found\n");
    exit(1);
  }

  LLVMGenericValueRef result = LLVMRunFunction(engine, main_func, 0, NULL);

  int exit_code = LLVMGenericValueToInt(result, 0);

  printf("\n[program exited with code %d]\n", exit_code);

  LLVMDisposeExecutionEngine(engine);
}
