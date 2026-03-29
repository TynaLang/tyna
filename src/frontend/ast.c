#include "ast.h"
#include <stdio.h>
#include <stdlib.h>

static AstNode *AstNode_new(AstKind ast_kind, Location loc) {
  AstNode *node = malloc(sizeof(AstNode));
  if (!node) {
    fprintf(stderr, "Failed to allocate AST node\n");
    exit(1);
  }
  node->tag = ast_kind;
  node->loc = loc;
  return node;
}

AstNode *AstNode_new_program(Location loc) {
  AstNode *node = AstNode_new(NODE_PROGRAM, loc);
  List_init(&node->program.children);
  return node;
}

AstNode *AstNode_new_let(AstNode *name, AstNode *value, TypeKind type,
                         Location loc) {
  AstNode *node = AstNode_new(NODE_LET, loc);
  node->let.name = name;
  node->let.value = value;
  node->let.declared_type = type;
  return node;
}

AstNode *AstNode_new_print(AstNode *value, Location loc) {
  AstNode *node = AstNode_new(NODE_PRINT, loc);
  node->print.value = value;
  return node;
}

AstNode *AstNode_new_number(double value, Location loc) {
  AstNode *node = AstNode_new(NODE_NUMBER, loc);
  node->number.value = value;
  return node;
}

AstNode *AstNode_new_char(char value, Location loc) {
  AstNode *node = AstNode_new(NODE_CHAR, loc);
  node->char_lit.value = value;
  return node;
}

AstNode *AstNode_new_string(char *value, Location loc) {
  AstNode *node = AstNode_new(NODE_STRING, loc);
  node->string.value = value;
  return node;
}

AstNode *AstNode_new_ident(char *value, Location loc) {
  AstNode *node = AstNode_new(NODE_IDENT, loc);
  node->ident.value = value;
  return node;
}

AstNode *AstNode_new_binary(AstNode *left, AstNode *right, BinaryOp op,
                            Location loc) {
  AstNode *node = AstNode_new(NODE_BINARY, loc);
  node->binary.left = left;
  node->binary.right = right;
  node->binary.op = op;
  return node;
}

AstNode *AstNode_new_unary(UnaryOp op, AstNode *expr, Location loc) {
  AstNode *node = AstNode_new(NODE_UNARY, loc);
  node->unary.expr = expr;
  node->unary.op = op;
  return node;
}

AstNode *AstNode_new_assign(AstNode *target, AstNode *value, Location loc) {
  AstNode *node = AstNode_new(NODE_ASSIGN, loc);
  node->assign.target = target;
  node->assign.value = value;
  return node;
}

AstNode *AstNode_new_expr_stmt(AstNode *expr, Location loc) {
  AstNode *node = AstNode_new(NODE_EXPR_STMT, loc);
  node->expr_stmt.expr = expr;
  return node;
}

AstNode *AstNode_new_call(AstNode *func, AstNode *arg, Location loc) {
  AstNode *node = AstNode_new(NODE_CALL, loc);
  node->call.func = func;
  node->call.arg = arg;
  return node;
}

static void print_indent(int indent) {
  for (int i = 0; i < indent; i++) {
    printf("  ");
  }
}

void Ast_print(AstNode *node, int indent) {
  if (!node)
    return;

  print_indent(indent);

  switch (node->tag) {
  case NODE_PROGRAM:
    printf("PROGRAM\n");
    for (size_t i = 0; i < node->program.children.len; i++) {
      AstNode *child = node->program.children.items[i];
      Ast_print(child, indent + 1);
    }
    break;
  case NODE_LET:
    printf("LET %s : %d\n", node->let.name->ident.value,
           node->let.declared_type);
    print_indent(indent + 1);
    printf("VALUE:\n");
    Ast_print(node->let.value, indent + 2);
    break;
  case NODE_PRINT:
    printf("PRINT\n");
    print_indent(indent + 1);
    printf("VALUE:\n");
    Ast_print(node->print.value, indent + 2);
    break;
  case NODE_NUMBER:
    printf("NUMBER: %f\n", node->number.value);
    break;
  case NODE_CHAR:
    printf("CHAR: %c\n", node->char_lit.value);
    break;
  case NODE_STRING:
    printf("STRING: %s\n", node->string.value);
    break;
  case NODE_IDENT:
    printf("IDENT: %s\n", node->ident.value);
    break;
  case NODE_BINARY:
    printf("BINARY OP: %d\n", node->binary.op);
    print_indent(indent + 1);
    printf("LEFT:\n");
    Ast_print(node->binary.left, indent + 2);
    print_indent(indent + 1);
    printf("RIGHT:\n");
    Ast_print(node->binary.right, indent + 2);
    break;
  case NODE_UNARY:
    printf("UNARY OP: %d\n", node->unary.op);
    print_indent(indent + 1);
    printf("EXPR:\n");
    Ast_print(node->unary.expr, indent + 2);
    break;
  case NODE_CALL:
    printf("CALL:\n");
    print_indent(indent + 1);
    printf("FUNC:\n");
    Ast_print(node->call.func, indent + 2);
    print_indent(indent + 1);
    printf("ARG:\n");
    Ast_print(node->call.arg, indent + 2);
    break;
  case NODE_EXPR_STMT:
    printf("EXPR_STMT\n");
    Ast_print(node->expr_stmt.expr, indent + 1);
    break;
  case NODE_ASSIGN:
    printf("ASSIGN\n");
    print_indent(indent + 1);
    printf("TARGET:\n");
    Ast_print(node->assign.target, indent + 2);
    print_indent(indent + 1);
    printf("VALUE:\n");
    Ast_print(node->assign.value, indent + 2);
    break;
  default:
    printf("UNKNOWN NODE\n");
    break;
  }
}
