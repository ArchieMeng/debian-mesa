#include "state_tracker/st_api.h"
#include "state_tracker/st_gl_api.h"

PUBLIC struct st_api *
st_api_create_OpenGL_ES2(void)
{
   return st_gl_api_create_es2();
}
