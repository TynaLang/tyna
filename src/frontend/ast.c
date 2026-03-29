#include "ast.h"
#include <stdio.h>
#include <stdlib.h>

static AstNode *AstNode_new(AstKind ast_kind) {
  AstNode *node = malloc(sizeof(AstNode));
  if (!node) {
    fprintf(stderr, "Failed to allocate AST node\n");
    exit(1);
  }
  node->tag = ast_kind;
  return node;
}

AstNode *AstNode_new_program(void) {
  AstNode *node = AstNode_new(NODE_PROGRAM);
  List_init(&node->program.children);
  return node;
}

AstNode *AstNode_new_let(AstNode *name, AstNode *value, TypeKind type) {
  AstNode *node = AstNode_new(NODE_LET);
  node->let.name = name;
  node->let.value = value;
  node->let.declared_type = type;
  return node;
}

AstNode *AstNode_new_print(AstNode *value) {
  AstNode *node = AstNode_new(NODE_PRINT);
  node->print.value = value;
  return node;
}

AstNode *AstNode_new_number(double value) {
  AstNode *node = AstNode_new(NODE_NUMBER);
  node->number.value = value;
  return node;
}

AstNode *AstNode_new_char(char value) {
  AstNode *node = AstNode_new(NODE_CHAR);
  node->char_lit.value = value;
  return node;
}

AstNode *AstNode_new_string(char *value) {
  AstNode *node = AstNode_new(NODE_STRING);
  node->string.value = value;
  return node;
}

AstNode *AstNode_new_ident(char *value) {
  AstNode *node = AstNode_new(NODE_IDENT);
  node->ident.value = value;
  return node;
}

AstNode *AstNode_new_binary(AstNode *left, AstNode *right, BinaryOp op) {
  AstNode *node = AstNode_new(NODE_BINARY);
  node->binary.left = left;
  node->binary.right = right;
  node->binary.op = op;
  return node;
}

AstNode *AstNode_new_unary(UnaryOp op, AstNode *expr) {
  AstNode *node = AstNode_new(NODE_UNARY);
  node->unary.expr = expr;
  node->unary.op = op;
  return node;
}

AstNode *AstNode_new_assign(AstNode *target, AstNode *value) {
  AstNode *node = AstNode_new(NODE_ASSIGN);
  node->assign.target = target;
  node->assign.value = value;
  return node;
}

AstNode *AstNode_new_expr_stmt(AstNode *expr) {
  AstNode *node = AstNode_new(NODE_EXPR_STMT);
  node->expr_stmt.expr = expr;
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
