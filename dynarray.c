#include "core.h"

#include "token.h"
#include "dynarray.h"

#include <stdio.h>
#include <stdlib.h>

IMPLEMENT_DYNAMIC_ARRAY_TYPE(token, (token){0})
IMPLEMENT_DYNAMIC_ARRAY_TYPE(char, 0)
IMPLEMENT_DYNAMIC_ARRAY_TYPE(stmt, (stmt){0})
IMPLEMENT_DYNAMIC_ARRAY_TYPE(str, NULL)
IMPLEMENT_DYNAMIC_ARRAY_TYPE(expr, (expr){0})