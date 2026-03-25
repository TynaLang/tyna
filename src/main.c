#include <stdio.h>
#include <stdlib.h>

#include "lexer.h"

char *read_file(const char *filepath);

int main(int argc, char **argv) {
  if (argc < 2) {
    printf("No entrypoint was provided. Exiting\n");
    return 1;
  }

  const char *src = read_file(argv[1]);

  Lexer lexer = make_lexer(src);

  while (1) {
    Token t = next_token(&lexer);

    printf("Token: type=%d, text='%s'", t.type, t.text ? t.text : "NULL");

    if (t.type == TOKEN_NUMBER)
      printf(", number=%le", t.number);

    if (t.line || t.col)
      printf(" [line=%zu, col=%zu]", t.line, t.col);

    printf("\n");

    if (t.text)
      free(t.text);

    if (t.type == TOKEN_EOF)
      break;
  }

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
