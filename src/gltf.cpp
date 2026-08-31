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

namespace gltf {

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

} // namespace gltf
