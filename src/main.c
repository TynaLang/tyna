#include <stdio.h>
#include <stdlib.h>

#include "lexer.h"
#include "parser.h"
#include "semantic.h"
#include "utils.h"

char *read_file(const char *filepath);
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
    printf("LET (type=%d)\n", node->let.declared_type);

    print_indent(indent + 1);
    printf("NAME:\n");
    Ast_print(node->let.name, indent + 2);

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
    printf("CHAR: '%c'\n", node->char_lit.value);
    break;

  case NODE_STRING:
    printf("STRING: \"%s\"\n", node->string.value);
    break;

  case NODE_IDENT:
    printf("IDENT: %s\n", node->ident.value);
    break;

  default:
    printf("UNKNOWN NODE\n");
    break;
  }
}

int main(int argc, char **argv) {
  if (argc < 2) {
    printf("No entrypoint was provided. Exiting\n");
    return 1;
  }

  const char *src = read_file(argv[1]);

  Lexer lexer = make_lexer(src);
  AstNode *program = Parser_process(&lexer);

  printf("\n=== AST ===\n");
  Ast_print(program, 0);

  SymbolTable table;
  SymbolTable_init(&table);

  analyze_program(program, &table);

  return 0;
}

char *read_file(const char *filename) {
  FILE *fp = fopen(filename, "rb");
  if (!fp) {
    perror("Error opening file");
    return NULL;
  }

  if (fseek(fp, 0, SEEK_END) != 0) {
    perror("fseek failed");
    fclose(fp);
    return NULL;
  }

  long filesize = ftell(fp);
  if (filesize < 0) {
    perror("ftell failed");
    fclose(fp);
    return NULL;
  }
  rewind(fp);

  char *buffer = malloc(filesize + 1);
  if (!buffer) {
    perror("malloc failed");
    fclose(fp);
    return NULL;
  }

  size_t read = fread(buffer, 1, filesize, fp);
  if (read != filesize) {
    perror("fread failed");
    free(buffer);
    fclose(fp);
    return NULL;
  }

  buffer[filesize] = '\0';
  fclose(fp);
  return buffer;
}
