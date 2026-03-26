#ifndef RUNNER_H
#define RUNNER_H

#include "codegen.h"

void Runner_verify(Codegen *cg);

void Runner_emit_ir(Codegen *cg, const char *filename);
void Runner_emit_object(Codegen *cg, const char *filename);
void Runner_link_executable(const char *obj, const char *out);

void Runner_jit(Codegen *cg);

#endif
