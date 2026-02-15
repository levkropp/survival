#ifndef EDIT_H
#define EDIT_H

#include "boot.h"

/* Launch the text editor on a file.
   path: CHAR16 directory path (e.g. L"\\notes")
   filename: ASCII filename (e.g. "todo.txt")
   If file doesn't exist, starts with empty document and creates on save. */
void edit_run(const CHAR16 *path, const char *filename);

#endif /* EDIT_H */
