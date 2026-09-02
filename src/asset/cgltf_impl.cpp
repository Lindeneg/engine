// the one translation unit that compiles cgltf

// cgltf uses fopen/strcpy, which MSVC deprecates; scoped to this file
#define _CRT_SECURE_NO_WARNINGS

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
