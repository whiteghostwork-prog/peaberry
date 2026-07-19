/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "load/tangent.h"

#include "mikktspace.h"

#include <volk.h>

/* User data passed to every mikktspace callback. The vertex array is the
 * interleaved pb_pbr_vertex buffer the loader has already populated with
 * positions, normals and UVs; mikktspace writes tangents back into the same
 * array in place. */
typedef struct {
    pb_pbr_vertex *vertices;
    uint32_t vertex_count;
    const void *indices;
    uint32_t index_count;
    VkIndexType index_type;
} tangent_ctx;

static uint32_t read_index(const tangent_ctx *ctx, uint32_t i)
{
    if (ctx->index_type == VK_INDEX_TYPE_UINT16) {
        const uint16_t *idx = ctx->indices;
        return idx[i];
    }
    const uint32_t *idx = ctx->indices;
    return idx[i];
}

static int mikk_get_num_faces(const SMikkTSpaceContext *pContext)
{
    const tangent_ctx *c = pContext->m_pUserData;
    /* Only triangle lists are produced by the glTF loader. */
    return (int)(c->index_count / 3);
}

static int mikk_get_num_vertices_of_face(const SMikkTSpaceContext *pContext, const int iFace)
{
    (void)pContext;
    (void)iFace;
    return 3;
}

static void mikk_get_position(const SMikkTSpaceContext *pContext, float fvPosOut[],
                              const int iFace, const int iVert)
{
    const tangent_ctx *c = pContext->m_pUserData;
    const uint32_t idx = read_index(c, (uint32_t)iFace * 3 + (uint32_t)iVert);
    const pb_pbr_vertex *v = (idx < c->vertex_count) ? &c->vertices[idx] : NULL;
    if (!v) {
        fvPosOut[0] = fvPosOut[1] = fvPosOut[2] = 0.0f;
        return;
    }
    fvPosOut[0] = v->pos[0];
    fvPosOut[1] = v->pos[1];
    fvPosOut[2] = v->pos[2];
}

static void mikk_get_normal(const SMikkTSpaceContext *pContext, float fvNormOut[],
                            const int iFace, const int iVert)
{
    const tangent_ctx *c = pContext->m_pUserData;
    const uint32_t idx = read_index(c, (uint32_t)iFace * 3 + (uint32_t)iVert);
    const pb_pbr_vertex *v = (idx < c->vertex_count) ? &c->vertices[idx] : NULL;
    if (!v) {
        fvNormOut[0] = fvNormOut[1] = 0.0f;
        fvNormOut[2] = 1.0f;
        return;
    }
    fvNormOut[0] = v->normal[0];
    fvNormOut[1] = v->normal[1];
    fvNormOut[2] = v->normal[2];
}

static void mikk_get_tex_coord(const SMikkTSpaceContext *pContext, float fvTexcOut[],
                               const int iFace, const int iVert)
{
    const tangent_ctx *c = pContext->m_pUserData;
    const uint32_t idx = read_index(c, (uint32_t)iFace * 3 + (uint32_t)iVert);
    const pb_pbr_vertex *v = (idx < c->vertex_count) ? &c->vertices[idx] : NULL;
    if (!v) {
        fvTexcOut[0] = fvTexcOut[1] = 0.0f;
        return;
    }
    fvTexcOut[0] = v->uv[0];
    fvTexcOut[1] = v->uv[1];
}

static void mikk_set_tspace_basic(const SMikkTSpaceContext *pContext, const float fvTangent[],
                                  const float fSign, const int iFace, const int iVert)
{
    tangent_ctx *c = pContext->m_pUserData;
    const uint32_t idx = read_index(c, (uint32_t)iFace * 3 + (uint32_t)iVert);
    if (idx >= c->vertex_count) {
        return;
    }
    pb_pbr_vertex *v = &c->vertices[idx];
    v->tangent[0] = fvTangent[0];
    v->tangent[1] = fvTangent[1];
    v->tangent[2] = fvTangent[2];
    /* Bitangent handedness in .w, reconstructed in the vertex shader as
     * cross(N, T) * tangent.w. Matches the mikktspace convention. */
    v->tangent[3] = fSign;
}

void pb_pbr_generate_tangents(
    pb_pbr_vertex *vertices,
    uint32_t vertex_count,
    const void *indices,
    uint32_t index_count,
    VkIndexType index_type)
{
    if (!vertices || vertex_count == 0 || !indices || index_count < 3) {
        return;
    }

    tangent_ctx ctx = {
        .vertices = vertices,
        .vertex_count = vertex_count,
        .indices = indices,
        .index_count = index_count,
        .index_type = index_type,
    };

    static const SMikkTSpaceInterface iface = {
        mikk_get_num_faces,
        mikk_get_num_vertices_of_face,
        mikk_get_position,
        mikk_get_normal,
        mikk_get_tex_coord,
        mikk_set_tspace_basic,
        NULL,
    };

    SMikkTSpaceContext mctx = {
        .m_pInterface = (SMikkTSpaceInterface *)&iface,
        .m_pUserData = &ctx,
    };

    genTangSpaceDefault(&mctx);
}
