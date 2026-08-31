#include "gltf.h"

#include <string.h>

#include <cgltf.h>

#include "file.h"
#include "logger.h"

static void *arena_alloc(void *user, cgltf_size size) {
    return memory::push((memory::Arena *)user, (size_t)size);
}

// no-op: the caller's temp scope reclaims the whole parse tree at once
static void arena_free(void *, void *) {}

// .glb is meant to be self-contained, so any external reference is a bug in the
// asset rather than something to silently go read off disk
static cgltf_result reject_external(const cgltf_memory_options *, const cgltf_file_options *,
                                    const char *path, cgltf_size *, void **) {
    LOG_ERR("gltf: asset references external file %s", path);
    return cgltf_result_file_not_found;
}

static void reject_release(const cgltf_memory_options *, const cgltf_file_options *, void *,
                           cgltf_size) {}

static const char *cgltf_err_str(cgltf_result r) {
    switch (r) {
    case cgltf_result_success:
        return "success";
    case cgltf_result_data_too_short:
        return "data too short";
    case cgltf_result_unknown_format:
        return "unknown format";
    case cgltf_result_invalid_json:
        return "invalid json";
    case cgltf_result_invalid_gltf:
        return "invalid gltf";
    case cgltf_result_invalid_options:
        return "invalid options";
    case cgltf_result_file_not_found:
        return "file not found";
    case cgltf_result_io_error:
        return "io error";
    case cgltf_result_out_of_memory:
        return "out of memory";
    case cgltf_result_legacy_gltf:
        return "legacy gltf";
    default:
        return "?";
    }
}

static const char *prim_type_str(cgltf_primitive_type t) {
    switch (t) {
    case cgltf_primitive_type_points:
        return "points";
    case cgltf_primitive_type_lines:
        return "lines";
    case cgltf_primitive_type_line_loop:
        return "line_loop";
    case cgltf_primitive_type_line_strip:
        return "line_strip";
    case cgltf_primitive_type_triangles:
        return "triangles";
    case cgltf_primitive_type_triangle_strip:
        return "tri_strip";
    case cgltf_primitive_type_triangle_fan:
        return "tri_fan";
    default:
        return "?";
    }
}

static const char *type_str(cgltf_type t) {
    switch (t) {
    case cgltf_type_scalar:
        return "scalar";
    case cgltf_type_vec2:
        return "vec2";
    case cgltf_type_vec3:
        return "vec3";
    case cgltf_type_vec4:
        return "vec4";
    case cgltf_type_mat2:
        return "mat2";
    case cgltf_type_mat3:
        return "mat3";
    case cgltf_type_mat4:
        return "mat4";
    default:
        return "?";
    }
}

static const char *comp_type_str(cgltf_component_type t) {
    switch (t) {
    case cgltf_component_type_r_8:
        return "i8";
    case cgltf_component_type_r_8u:
        return "u8";
    case cgltf_component_type_r_16:
        return "i16";
    case cgltf_component_type_r_16u:
        return "u16";
    case cgltf_component_type_r_32u:
        return "u32";
    case cgltf_component_type_r_32f:
        return "f32";
    default:
        return "?";
    }
}

static const char *safe_name(const char *name) { return name ? name : "<unnamed>"; }

#define NAMES_PER_LINE 4

static void log_names(const char *prefix, const char **names, u64 count) {
    char line[512];
    u64 col = 0;
    u64 off = 0;

    for (u64 i = 0; i < count; i++) {
        i32 n = snprintf(line + off, sizeof(line) - off, "%s%s", col ? ", " : "", names[i]);
        if (n > 0 && (u64)n < sizeof(line) - off)
            off += (u64)n;

        if (++col == NAMES_PER_LINE || i + 1 == count) {
            LOG_INFO("%s%s", prefix, line);
            col = 0;
            off = 0;
            line[0] = 0;
        }
    }
}

static void dump_accessor(const char *label, const cgltf_accessor *a) {
    if (!a) {
        LOG_INFO("gltf:       %-12s <none>", label);
        return;
    }
    LOG_INFO("gltf:       %-12s %-6s %-3s x%llu%s", label, type_str(a->type),
             comp_type_str(a->component_type), (unsigned long long)a->count,
             a->is_sparse ? " SPARSE" : "");
}

static void dump_meshes(const cgltf_data *d) {
    for (cgltf_size i = 0; i < d->meshes_count; i++) {
        const cgltf_mesh *m = &d->meshes[i];
        LOG_INFO("gltf:   mesh[%llu] \"%s\" primitives=%llu targets=%llu", (unsigned long long)i,
                 safe_name(m->name), (unsigned long long)m->primitives_count,
                 (unsigned long long)m->target_names_count);

        for (cgltf_size p = 0; p < m->primitives_count; p++) {
            const cgltf_primitive *prim = &m->primitives[p];
            LOG_INFO("gltf:     prim[%llu] %s material=\"%s\"%s", (unsigned long long)p,
                     prim_type_str(prim->type),
                     prim->material ? safe_name(prim->material->name) : "<none>",
                     prim->has_draco_mesh_compression ? " DRACO" : "");

            dump_accessor("INDICES", prim->indices);
            for (cgltf_size a = 0; a < prim->attributes_count; a++)
                dump_accessor(safe_name(prim->attributes[a].name), prim->attributes[a].data);
        }
    }
}

static void dump_skins(memory::Arena *arena, const cgltf_data *d) {
    for (cgltf_size i = 0; i < d->skins_count; i++) {
        const cgltf_skin *s = &d->skins[i];
        LOG_INFO("gltf:   skin[%llu] \"%s\" joints=%llu ibm=%s root=\"%s\"", (unsigned long long)i,
                 safe_name(s->name), (unsigned long long)s->joints_count,
                 s->inverse_bind_matrices ? "yes" : "no",
                 s->skeleton ? safe_name(s->skeleton->name) : "<none>");

        const char **names = push_array(arena, const char *, s->joints_count);
        for (cgltf_size j = 0; j < s->joints_count; j++)
            names[j] = s->joints[j] ? safe_name(s->joints[j]->name) : "<null>";
        log_names("gltf:     ", names, s->joints_count);
    }
}

static void dump_animations(memory::Arena *arena, const cgltf_data *d) {
    for (cgltf_size i = 0; i < d->animations_count; i++) {
        const cgltf_animation *a = &d->animations[i];

        u32 paths[cgltf_animation_path_type_max_enum] = {};
        u32 interps[cgltf_interpolation_type_max_enum] = {};
        f32 duration = 0.0f;

        for (cgltf_size c = 0; c < a->channels_count; c++) {
            const cgltf_animation_channel *ch = &a->channels[c];
            if (ch->target_path < cgltf_animation_path_type_max_enum)
                paths[ch->target_path]++;
            if (ch->sampler) {
                if (ch->sampler->interpolation < cgltf_interpolation_type_max_enum)
                    interps[ch->sampler->interpolation]++;
                const cgltf_accessor *in = ch->sampler->input;
                if (in && in->has_max && in->max[0] > duration)
                    duration = in->max[0];
            }
        }

        LOG_INFO("gltf:   anim[%llu] \"%s\" channels=%llu samplers=%llu duration=%.3fs",
                 (unsigned long long)i, safe_name(a->name), (unsigned long long)a->channels_count,
                 (unsigned long long)a->samplers_count, (f64)duration);
        LOG_INFO("gltf:     translate=%u rotate=%u scale=%u weights=%u",
                 paths[cgltf_animation_path_type_translation],
                 paths[cgltf_animation_path_type_rotation],
                 paths[cgltf_animation_path_type_scale],
                 paths[cgltf_animation_path_type_weights]);
        LOG_INFO("gltf:     linear=%u step=%u cubic=%u", interps[cgltf_interpolation_type_linear],
                 interps[cgltf_interpolation_type_step],
                 interps[cgltf_interpolation_type_cubic_spline]);

        // channels bind to nodes by name across files, so the distinct target set
        // is what a clip can actually drive
        const char **targets = push_array(arena, const char *, a->channels_count);
        cgltf_size unique = 0;
        for (cgltf_size c = 0; c < a->channels_count; c++) {
            const char *n =
                a->channels[c].target_node ? safe_name(a->channels[c].target_node->name) : "<null>";
            b32 seen = false;
            for (cgltf_size k = 0; k < unique && !seen; k++)
                seen = strcmp(targets[k], n) == 0;
            if (!seen)
                targets[unique++] = n;
        }

        LOG_INFO("gltf:     %llu distinct target nodes", (unsigned long long)unique);
        log_names("gltf:     ", targets, unique);
    }
}

static void dump_images(const cgltf_data *d) {
    for (cgltf_size i = 0; i < d->images_count; i++) {
        const cgltf_image *img = &d->images[i];
        LOG_INFO("gltf:   image[%llu] \"%s\" mime=\"%s\" embedded=%s uri=%s",
                 (unsigned long long)i, safe_name(img->name), img->mime_type ? img->mime_type : "?",
                 img->buffer_view ? "yes" : "no", img->uri ? img->uri : "<none>");
    }
}

// Parse + load_buffers + validate. The returned tree lives in `scratch` and dies
// with the caller's temp scope.
static result::Error parse_glb(memory::Arena *scratch, const char *path, cgltf_data **out) {
    file::BlobResult blob = file::read(scratch, path);
    if (blob.err != result::OK)
        return blob.err;

    cgltf_options opts = {};
    opts.memory.alloc_func = arena_alloc;
    opts.memory.free_func = arena_free;
    opts.memory.user_data = scratch;
    opts.file.read = reject_external;
    opts.file.release = reject_release;

    cgltf_result r = cgltf_parse(&opts, blob.val.data, blob.val.size, out);
    if (r != cgltf_result_success) {
        LOG_ERR("gltf: parse failed on %s (%s)", path, cgltf_err_str(r));
        return result::ERR_PARSE;
    }

    r = cgltf_load_buffers(&opts, *out, path);
    if (r != cgltf_result_success) {
        LOG_ERR("gltf: load_buffers failed on %s (%s)", path, cgltf_err_str(r));
        return result::ERR_PARSE;
    }

    r = cgltf_validate(*out);
    if (r != cgltf_result_success) {
        LOG_ERR("gltf: validate failed on %s (%s)", path, cgltf_err_str(r));
        return result::ERR_INVALID;
    }

    return result::OK;
}

static void read_floats(const cgltf_accessor *a, cgltf_size i, f32 *out, cgltf_size n) {
    if (!a || !cgltf_accessor_read_float(a, i, out, n))
        memset(out, 0, n * sizeof(f32));
}

static void read_joints(const cgltf_accessor *a, cgltf_size i, u16 *out) {
    cgltf_uint tmp[4] = {};
    if (a)
        cgltf_accessor_read_uint(a, i, tmp, 4);
    for (u32 k = 0; k < 4; k++)
        out[k] = (u16)tmp[k];
}

static const cgltf_accessor *attr(const cgltf_primitive *p, cgltf_attribute_type t) {
    return cgltf_find_accessor(p, t, 0);
}

// A run of vertices shared by one or more primitives. pistol.glb points all 47
// of its primitives at the same accessors, so keying on the full attribute set
// collapses 146k duplicated vertices back down to 3120.
struct VertexBlock {
    const cgltf_accessor *pos;
    const cgltf_accessor *nrm;
    const cgltf_accessor *uv;
    const cgltf_accessor *joints;
    const cgltf_accessor *weights;
    u32 base;
    u32 count;
};

static b32 block_matches(const VertexBlock *b, const cgltf_primitive *p) {
    return b->pos == attr(p, cgltf_attribute_type_position) &&
           b->nrm == attr(p, cgltf_attribute_type_normal) &&
           b->uv == attr(p, cgltf_attribute_type_texcoord) &&
           b->joints == attr(p, cgltf_attribute_type_joints) &&
           b->weights == attr(p, cgltf_attribute_type_weights);
}

static linalg::Vec3 xform_point(linalg::Mat4 m, linalg::Vec3 p) {
    linalg::Vec4 r = linalg::mat4_mul_vec4(m, linalg::Vec4{p.x, p.y, p.z, 1.0f});
    return linalg::Vec3{r.x, r.y, r.z};
}

static linalg::Vec3 xform_dir(linalg::Mat4 m, linalg::Vec3 v) {
    linalg::Vec4 r = linalg::mat4_mul_vec4(m, linalg::Vec4{v.x, v.y, v.z, 0.0f});
    linalg::Vec3 d{r.x, r.y, r.z};
    f32 len = linalg::vec3_length(d);
    return len > 1e-8f ? linalg::vec3_scale(d, 1.0f / len) : linalg::Vec3{0.0f, 1.0f, 0.0f};
}

static void fill_block(asset::Mesh *mesh, const VertexBlock *b, linalg::Mat4 bake) {
    for (u32 i = 0; i < b->count; i++) {
        f32 pos[3], nrm[3], uv[2];
        read_floats(b->pos, i, pos, 3);
        read_floats(b->nrm, i, nrm, 3);
        read_floats(b->uv, i, uv, 2);

        linalg::Vec3 p = xform_point(bake, linalg::Vec3{pos[0], pos[1], pos[2]});
        linalg::Vec3 n = xform_dir(bake, linalg::Vec3{nrm[0], nrm[1], nrm[2]});

        if (mesh->vertex_kind == asset::VERTEX_SKINNED) {
            asset::VertexSkinned *v = &mesh->vertices.skinned[b->base + i];
            v->pos = p;
            v->normal = n;
            v->uv = linalg::Vec2{uv[0], uv[1]};
            read_joints(b->joints, i, v->joints);
            if (b->weights) {
                read_floats(b->weights, i, v->weights, 4);
            } else {
                // an unskinned primitive inside a skinned mesh rides joint 0
                v->weights[0] = 1.0f;
            }
        } else {
            asset::VertexStatic *v = &mesh->vertices.statics[b->base + i];
            v->pos = p;
            v->normal = n;
            v->uv = linalg::Vec2{uv[0], uv[1]};
        }
    }
}

static void load_mesh(asset::Manager *m, asset::Scope s, memory::Arena *scratch,
                      const cgltf_data *d, const cgltf_mesh *src,
                      const asset::MaterialHandle *materials, linalg::Mat4 bake,
                      asset::Mesh *mesh) {
    memory::ArenaTemp t = memory::temp_begin(scratch);
    memory::Arena *out = asset::scope_arena(m, s);

    VertexBlock *blocks = push_array(scratch, VertexBlock, src->primitives_count);
    u32 block_count = 0;
    u32 total_verts = 0;
    u32 total_indices = 0;
    b32 skinned = false;

    for (cgltf_size p = 0; p < src->primitives_count; p++) {
        const cgltf_primitive *prim = &src->primitives[p];
        const cgltf_accessor *pos = attr(prim, cgltf_attribute_type_position);
        if (!pos || prim->type != cgltf_primitive_type_triangles)
            continue;

        if (attr(prim, cgltf_attribute_type_joints))
            skinned = true;

        b32 found = false;
        for (u32 b = 0; b < block_count && !found; b++)
            found = block_matches(&blocks[b], prim);

        if (!found) {
            blocks[block_count] = VertexBlock{
                .pos = pos,
                .nrm = attr(prim, cgltf_attribute_type_normal),
                .uv = attr(prim, cgltf_attribute_type_texcoord),
                .joints = attr(prim, cgltf_attribute_type_joints),
                .weights = attr(prim, cgltf_attribute_type_weights),
                .base = total_verts,
                .count = (u32)pos->count,
            };
            total_verts += (u32)pos->count;
            block_count++;
        }

        total_indices += (u32)(prim->indices ? prim->indices->count : pos->count);
    }

    mesh->vertex_kind = skinned ? asset::VERTEX_SKINNED : asset::VERTEX_STATIC;
    mesh->vertex_count = total_verts;
    if (skinned)
        mesh->vertices.skinned = push_array(out, asset::VertexSkinned, total_verts);
    else
        mesh->vertices.statics = push_array(out, asset::VertexStatic, total_verts);

    mesh->indices = push_array(out, u32, total_indices);
    mesh->submeshes = push_array(out, asset::Submesh, src->primitives_count);

    for (u32 b = 0; b < block_count; b++)
        fill_block(mesh, &blocks[b], bake);

    u32 index_cursor = 0;
    for (cgltf_size p = 0; p < src->primitives_count; p++) {
        const cgltf_primitive *prim = &src->primitives[p];
        const cgltf_accessor *pos = attr(prim, cgltf_attribute_type_position);
        if (!pos || prim->type != cgltf_primitive_type_triangles)
            continue;

        u32 base = 0;
        for (u32 b = 0; b < block_count; b++) {
            if (block_matches(&blocks[b], prim)) {
                base = blocks[b].base;
                break;
            }
        }

        u32 start = index_cursor;
        u32 count = (u32)(prim->indices ? prim->indices->count : pos->count);
        for (u32 i = 0; i < count; i++) {
            u32 v = prim->indices ? (u32)cgltf_accessor_read_index(prim->indices, i) : i;
            mesh->indices[index_cursor++] = base + v;
        }

        asset::MaterialHandle mat = asset::MATERIAL_FALLBACK;
        if (prim->material)
            mat = materials[cgltf_material_index(d, prim->material)];

        mesh->submeshes[mesh->submesh_count++] =
            asset::Submesh{.index_offset = start, .index_count = count, .material = mat};
    }

    mesh->index_count = index_cursor;
    memory::temp_end(t);
}

static asset::SkeletonHandle load_skeleton(asset::Manager *m, asset::Scope s,
                                           const cgltf_skin *skin) {
    asset::SkeletonHandle h{
        OR_ELSE(asset::create_skeleton(m, s), asset::SKELETON_FALLBACK)};
    if (h.v == 0)
        return h;

    memory::Arena *out = asset::scope_arena(m, s);
    asset::Skeleton *skel = asset::get(m, h);

    skel->joint_count = (u32)skin->joints_count;
    skel->joints = push_array(out, asset::Joint, skel->joint_count);
    skel->by_name = strmap::create(out, skel->joint_count);
    skel->root = linalg::mat4_identity();

    b32 have_root = false;

    for (u32 j = 0; j < skel->joint_count; j++) {
        const cgltf_node *node = skin->joints[j];
        asset::Joint *joint = &skel->joints[j];

        gltf::canonical_name(joint->name, ASSET_NAME_CAP, node ? node->name : NULL);

        joint->parent = -1;
        if (node && node->parent) {
            for (cgltf_size k = 0; k < skin->joints_count; k++) {
                if (skin->joints[k] == node->parent) {
                    joint->parent = (i32)k;
                    break;
                }
            }

            // The node has a parent that is not itself a joint -- an armature
            // or scene node above the skin. The inverse bind matrices include
            // it, so we have to as well.
            if (joint->parent < 0 && !have_root) {
                cgltf_node_transform_world(node->parent, skel->root.m);
                have_root = true;
                LOG_INFO("gltf: skeleton root above \"%s\" is \"%s\"", joint->name,
                         safe_name(node->parent->name));
            }
        }

        if (skin->inverse_bind_matrices)
            read_floats(skin->inverse_bind_matrices, j, joint->inverse_bind.m, 16);
        else
            joint->inverse_bind = linalg::mat4_identity();

        joint->bind_t = linalg::Vec3{};
        joint->bind_r = linalg::quat_identity();
        joint->bind_s = linalg::Vec3{1.0f, 1.0f, 1.0f};
        if (node) {
            if (node->has_translation)
                joint->bind_t = linalg::Vec3{node->translation[0], node->translation[1],
                                             node->translation[2]};
            if (node->has_rotation)
                joint->bind_r = linalg::Quat{node->rotation[0], node->rotation[1],
                                             node->rotation[2], node->rotation[3]};
            if (node->has_scale)
                joint->bind_s = linalg::Vec3{node->scale[0], node->scale[1], node->scale[2]};
        }

        strmap::insert(&skel->by_name, joint->name, j);
    }

    return h;
}

namespace gltf {

void canonical_name(char *dst, u64 cap, const char *src) {
    ASSERT(dst && cap);

    u64 i = 0;
    if (src) {
        for (; src[i] && i + 1 < cap; i++) {
            char c = src[i];
            b32 alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            dst[i] = alnum ? c : '_';
        }
    }
    dst[i] = 0;
}

result::Error inspect(memory::Arena *scratch, const char *path) {
    memory::ArenaTemp t = memory::temp_begin(scratch);

    file::BlobResult blob = file::read(scratch, path);
    if (blob.err != result::OK) {
        memory::temp_end(t);
        return blob.err;
    }

    cgltf_options opts = {};
    opts.memory.alloc_func = arena_alloc;
    opts.memory.free_func = arena_free;
    opts.memory.user_data = scratch;
    opts.file.read = reject_external;
    opts.file.release = reject_release;

    cgltf_data *d = NULL;
    cgltf_result r = cgltf_parse(&opts, blob.val.data, blob.val.size, &d);
    if (r != cgltf_result_success) {
        LOG_ERR("gltf: parse failed on %s (%s)", path, cgltf_err_str(r));
        memory::temp_end(t);
        return result::ERR_PARSE;
    }

    r = cgltf_load_buffers(&opts, d, path);
    if (r != cgltf_result_success) {
        LOG_ERR("gltf: load_buffers failed on %s (%s)", path, cgltf_err_str(r));
        memory::temp_end(t);
        return result::ERR_PARSE;
    }

    r = cgltf_validate(d);
    if (r != cgltf_result_success) {
        LOG_ERR("gltf: validate failed on %s (%s)", path, cgltf_err_str(r));
        memory::temp_end(t);
        return result::ERR_INVALID;
    }

    LOG_INFO("gltf: %s (%.1f KiB, %s)", path, (f64)blob.val.size / 1024.0,
             d->file_type == cgltf_file_type_glb ? "glb" : "gltf");
    LOG_INFO("gltf:   generator=\"%s\" version=\"%s\"",
             d->asset.generator ? d->asset.generator : "?",
             d->asset.version ? d->asset.version : "?");
    LOG_INFO("gltf:   scenes=%llu nodes=%llu meshes=%llu materials=%llu textures=%llu images=%llu",
             (unsigned long long)d->scenes_count, (unsigned long long)d->nodes_count,
             (unsigned long long)d->meshes_count, (unsigned long long)d->materials_count,
             (unsigned long long)d->textures_count, (unsigned long long)d->images_count);
    LOG_INFO("gltf:   skins=%llu animations=%llu buffers=%llu",
             (unsigned long long)d->skins_count, (unsigned long long)d->animations_count,
             (unsigned long long)d->buffers_count);

    dump_meshes(d);
    dump_skins(scratch, d);
    dump_animations(scratch, d);
    dump_images(d);

    // no cgltf_free: free_func is a no-op and temp_end reclaims everything
    memory::temp_end(t);
    return result::OK;
}

asset::ModelResult load_model(asset::Manager *m, asset::Scope s, const char *abs_path,
                              const char *name) {
    ASSERT(m && abs_path);

    memory::Arena *scratch = asset::scratch_arena(m);
    memory::ArenaTemp t = memory::temp_begin(scratch);

    cgltf_data *d = NULL;
    result::Error e = parse_glb(scratch, abs_path, &d);
    if (e != result::OK) {
        memory::temp_end(t);
        return asset::ModelResult{.err = e};
    }

    asset::MaterialHandle *materials =
        push_array(scratch, asset::MaterialHandle, d->materials_count);
    for (cgltf_size i = 0; i < d->materials_count; i++) {
        const cgltf_material *src = &d->materials[i];
        materials[i] = OR_ELSE(asset::create_material(m, s), asset::MATERIAL_FALLBACK);

        asset::Material *mat = asset::get(m, materials[i]);
        snprintf(mat->name, ASSET_NAME_CAP, "%s", src->name ? src->name : "<unnamed>");

        if (src->has_pbr_metallic_roughness) {
            const cgltf_pbr_metallic_roughness *pbr = &src->pbr_metallic_roughness;
            mat->base_color = linalg::Vec4{pbr->base_color_factor[0], pbr->base_color_factor[1],
                                           pbr->base_color_factor[2], pbr->base_color_factor[3]};
            mat->metallic = pbr->metallic_factor;
            mat->roughness = pbr->roughness_factor;
        } else {
            mat->base_color = linalg::Vec4{1.0f, 1.0f, 1.0f, 1.0f};
            mat->roughness = 1.0f;
        }
    }

    asset::SkeletonHandle skel = asset::SKELETON_FALLBACK;
    if (d->skins_count > 0) {
        if (d->skins_count > 1)
            LOG_WARN("gltf: %s has %llu skins, using the first", abs_path,
                     (unsigned long long)d->skins_count);
        skel = load_skeleton(m, s, &d->skins[0]);
    }

    asset::ModelResult mr = asset::create_model(m, s, name);
    if (mr.err != result::OK) {
        memory::temp_end(t);
        return mr;
    }

    // Geometry hangs off nodes, not off the mesh list: the node carries the
    // transform, and the same mesh can appear under several nodes.
    u32 node_meshes = 0;
    for (cgltf_size i = 0; i < d->nodes_count; i++)
        if (d->nodes[i].mesh)
            node_meshes++;

    // pool slots are fixed arrays, so this stays valid across further creates
    asset::Model *model = asset::get(m, mr.val);
    model->skeleton = skel;
    model->meshes = push_array(asset::scope_arena(m, s), asset::MeshHandle, node_meshes);

    u32 verts = 0;
    u32 indices = 0;
    for (cgltf_size i = 0; i < d->nodes_count; i++) {
        const cgltf_node *node = &d->nodes[i];
        if (!node->mesh)
            continue;

        // A skinned mesh's node transform MUST be ignored per the glTF spec --
        // the joints define its space. Static geometry bakes the node's world
        // transform in, so vertices are model-space and physics needs no extra
        // matrix per mesh.
        linalg::Mat4 bake = linalg::mat4_identity();
        if (!node->skin)
            cgltf_node_transform_world(node, bake.m);

        asset::MeshHandle mh = OR_ELSE(asset::create_mesh(m, s), asset::MESH_FALLBACK);
        model->meshes[model->mesh_count++] = mh;
        if (mh.v == 0)
            continue;

        asset::Mesh *mesh = asset::get(m, mh);
        load_mesh(m, s, scratch, d, node->mesh, materials, bake, mesh);
        verts += mesh->vertex_count;
        indices += mesh->index_count;
    }

    LOG_INFO("gltf: loaded %s -- %u mesh node(s), %u verts, %u indices, %llu materials, %s", name,
             model->mesh_count, verts, indices, (unsigned long long)d->materials_count,
             skel.v ? "skinned" : "static");

    memory::temp_end(t);
    return mr;
}

asset::ClipResult load_clip(asset::Manager *m, asset::Scope s, const char *abs_path,
                            asset::SkeletonHandle target) {
    ASSERT(m && abs_path);

    memory::Arena *scratch = asset::scratch_arena(m);
    memory::ArenaTemp t = memory::temp_begin(scratch);

    cgltf_data *d = NULL;
    result::Error e = parse_glb(scratch, abs_path, &d);
    if (e != result::OK) {
        memory::temp_end(t);
        return asset::ClipResult{.err = e};
    }

    if (d->animations_count == 0) {
        LOG_ERR("gltf: %s contains no animations", abs_path);
        memory::temp_end(t);
        return asset::ClipResult{.err = result::ERR_INVALID};
    }
    if (d->animations_count > 1)
        LOG_WARN("gltf: %s has %llu animations, using the first", abs_path,
                 (unsigned long long)d->animations_count);

    const cgltf_animation *src = &d->animations[0];

    // Cubic spline packs three values per key. Reading it as linear would
    // silently produce garbage, so refuse rather than guess.
    for (cgltf_size c = 0; c < src->channels_count; c++) {
        const cgltf_animation_sampler *smp = src->channels[c].sampler;
        if (smp && smp->interpolation == cgltf_interpolation_type_cubic_spline) {
            LOG_ERR("gltf: %s uses cubic spline interpolation, which is not supported", abs_path);
            memory::temp_end(t);
            return asset::ClipResult{.err = result::ERR_UNSUPPORTED};
        }
    }

    asset::ClipResult cr = asset::create_clip(m, s);
    if (cr.err != result::OK) {
        memory::temp_end(t);
        return cr;
    }

    memory::Arena *out = asset::scope_arena(m, s);
    asset::Clip *clip = asset::get(m, cr.val);
    asset::Skeleton *skel = asset::get(m, target);

    snprintf(clip->name, ASSET_NAME_CAP, "%s", src->name ? src->name : "<unnamed>");
    clip->channels = push_array(out, asset::Channel, src->channels_count);
    clip->rotation_only = d->skins_count == 0;

    f32 max_clip_t = 0.0f;   // largest translation the clip asks for
    f32 max_bind_t = 0.0f;   // largest the target skeleton actually uses

    for (cgltf_size c = 0; c < src->channels_count; c++) {
        const cgltf_animation_channel *ch = &src->channels[c];
        if (!ch->sampler || !ch->sampler->input || !ch->sampler->output)
            continue;

        asset::Path path;
        switch (ch->target_path) {
        case cgltf_animation_path_type_translation:
            path = asset::PATH_TRANSLATION;
            break;
        case cgltf_animation_path_type_rotation:
            path = asset::PATH_ROTATION;
            break;
        case cgltf_animation_path_type_scale:
            path = asset::PATH_SCALE;
            break;
        default:
            continue; // morph weights: no morph targets in our assets
        }

        asset::Channel *dst = &clip->channels[clip->channel_count++];
        dst->path = path;
        dst->interp = ch->sampler->interpolation == cgltf_interpolation_type_step
                          ? asset::INTERP_STEP
                          : asset::INTERP_LINEAR;

        gltf::canonical_name(dst->target, ASSET_NAME_CAP,
                             ch->target_node ? ch->target_node->name : NULL);

        const cgltf_accessor *in = ch->sampler->input;
        const cgltf_accessor *val = ch->sampler->output;
        u32 comps = path == asset::PATH_ROTATION ? 4 : 3;

        dst->key_count = (u32)in->count;
        dst->times = push_array(out, f32, dst->key_count);
        dst->values = push_array(out, f32, (u64)dst->key_count * comps);
        for (u32 k = 0; k < dst->key_count; k++) {
            read_floats(in, k, &dst->times[k], 1);
            read_floats(val, k, &dst->values[(u64)k * comps], comps);
        }

        if (path == asset::PATH_TRANSLATION) {
            for (u32 k = 0; k < dst->key_count; k++) {
                const f32 *v = &dst->values[(u64)k * comps];
                max_clip_t = MAX(max_clip_t,
                                 linalg::vec3_length(linalg::Vec3{v[0], v[1], v[2]}));
            }
        }

        if (in->has_max && in->max[0] > clip->duration)
            clip->duration = in->max[0];

        u32 *joint = strmap::find(&skel->by_name, dst->target);
        dst->joint = joint ? (i32)*joint : -1;
        if (joint)
            clip->bound_count++;
    }

    for (u32 j = 0; j < skel->joint_count; j++)
        max_bind_t = MAX(max_bind_t, linalg::vec3_length(skel->joints[j].bind_t));

    if (clip->bound_count == 0)
        LOG_ERR("gltf: %s bound 0 of %u channels to skeleton -- name mismatch, clip will not "
                "animate anything",
                abs_path, clip->channel_count);
    else
        LOG_INFO("gltf: loaded clip \"%s\" -- %.3fs, bound %u of %u channels, %s", clip->name,
                 (f64)clip->duration, clip->bound_count, clip->channel_count,
                 clip->rotation_only ? "retargeted (rotation only)" : "native rig");

    // If these disagree by a large factor the two rigs are in different units,
    // which is exactly why translations cannot be transferred.
    LOG_INFO("gltf:   max translation: clip %.4f, target skeleton %.4f", (f64)max_clip_t,
             (f64)max_bind_t);

    memory::temp_end(t);
    return cr;
}

} // namespace gltf
