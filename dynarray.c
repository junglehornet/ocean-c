#include "core.h"

#include "dynarray.h"

#include <stdlib.h>
#include <stdio.h>

token emptyToken;
IMPLEMENT_DYNAMIC_ARRAY_TYPE(token, emptyToken)
IMPLEMENT_DYNAMIC_ARRAY_TYPE(char, 0)
