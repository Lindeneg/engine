// the single translation unit that compiles cgltf; everything else just includes
// the header for declarations

// cgltf uses fopen/strcpy, which MSVC's CRT deprecates. Scoped to this file so
// our own code keeps the warnings.
#define _CRT_SECURE_NO_WARNINGS

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
