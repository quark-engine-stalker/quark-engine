#include "stdafx.h"
#include "light.h"

IC bool pred_LI(const light_indirect& A, const light_indirect& B)
{
    return A.E > B.E;
}

void light::gi_generate()
{
    indirect.clear();
    indirect_photons = ps_r2_ls_flags.test(R2FLAG_GI) ? ps_r2_GI_photons : 0;

    CRandom random;
    random.seed(0x12071980);

    struct gi_query_workspace
    {
        CDB::COLLIDER collider;

        gi_query_workspace()
        {
            collider.r_reserve(1);
        }
    };
    thread_local gi_query_workspace workspace;
    CDB::COLLIDER& xrc = workspace.collider;
    CDB::MODEL* model = g_pGameLevel->ObjectSpace.GetStaticModel();
    CDB::TRI* tris = g_pGameLevel->ObjectSpace.GetStaticTris();
    Fvector* verts = g_pGameLevel->ObjectSpace.GetStaticVerts();

    xrc.ray_options(CDB::OPT_CULL | CDB::OPT_ONLYNEAREST);

    for (int i = 0; i < static_cast<int>(indirect_photons * 8); ++i)
    {
        Fvector dir, idir;

        switch (flags.type)
        {
        case IRender_Light::POINT:
            dir.random_dir(random);
            break;
        case IRender_Light::SPOT:
        case IRender_Light::OMNIPART:
            dir.random_dir(direction, cone, random);
            break;
        default:
            continue;
        }

        dir.normalize();
        xrc.ray_query(model, position, dir, range);

        if (!xrc.r_count())
            continue;

        CDB::RESULT* result = xrc.r_begin();
        CDB::TRI& triangle = tris[result->id];
        Fvector triangle_vertices[3] =
        {
            verts[triangle.verts[0]],
            verts[triangle.verts[1]],
            verts[triangle.verts[2]]
        };

        Fvector normal;
        normal.mknormal(triangle_vertices[0], triangle_vertices[1], triangle_vertices[2]);

        light_indirect indirect_light;
        indirect_light.P.mad(position, dir, result->range);
        indirect_light.D.reflect(dir, normal);
        indirect_light.E = normal.dotproduct(idir.invert(dir)) * (1.0f - result->range / range);

        if (indirect_light.E < ps_r2_GI_clip)
            continue;

        indirect_light.S = spatial.sector;
        indirect.push_back(indirect_light);
    }

    std::sort(indirect.begin(), indirect.end(), pred_LI);

    if (indirect.size() > indirect_photons)
        indirect.erase(indirect.begin() + indirect_photons, indirect.end());

    if (!indirect.empty())
    {
        float total_energy = 0.0f;
        for (u32 i = 0; i < indirect.size(); ++i)
            total_energy += indirect[i].E;

        if (total_energy > EPS_S)
        {
            const float energy_scale = ps_r2_GI_refl / total_energy;
            for (u32 i = 0; i < indirect.size(); ++i)
                indirect[i].E *= energy_scale;
        }
    }
}
